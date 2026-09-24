/*
 * rdma_write_bw.c - RDMA Write one-sided bandwidth benchmark
 *
 * Uses RC (Reliable Connected) QPs with IBV_WR_RDMA_WRITE operations.
 * Server is fully passive during the data path (CPU not involved).
 * QP info and remote MR info are exchanged out-of-band over a TCP socket.
 *
 * Usage:
 *   Server (VM1): ./rdma_write_bw [options]
 *   Client (VM2): ./rdma_write_bw [options] <server-ip>
 *
 * Example (IB mode):
 *   VM1: ./rdma_write_bw -d mlx5_0 -i 1
 *   VM2: ./rdma_write_bw -d mlx5_0 -i 1 192.168.1.10
 *
 * Example (RoCE mode, GID index required):
 *   VM1: ./rdma_write_bw -d mlx5_0 -i 1 -g 3
 *   VM2: ./rdma_write_bw -d mlx5_0 -i 1 -g 3 192.168.1.10
 *
 * Tip: find your GID index with: ibv_devinfo -v | grep -i gid
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <infiniband/verbs.h>

/* ── defaults ────────────────────────────────────────────────────────── */
#define DEFAULT_TCP_PORT  18516
#define DEFAULT_ITERS     5000
#define DEFAULT_MSG_SIZE  65536   /* 64 KB */
#define DEFAULT_IB_PORT   1
#define DEFAULT_GID_IDX   -1     /* -1 = IB/LID mode; ≥0 = RoCE/GID mode */
#define BASE_PSN          0x123456
#define TX_DEPTH          128
#define CQ_MOD            32

/* ── data structures ─────────────────────────────────────────────────── */

/* Peer QP and memory-region metadata exchanged for RDMA WRITEs. */
struct peer_info {
    uint32_t qpn;          /* Queue pair number used to address the peer QP. */
    uint32_t psn;          /* Packet sequence number used to initialize the QP. */
    uint16_t lid;          /* Local identifier used for InfiniBand routing. */
    uint8_t  gid[16];      /* Global identifier used for RoCE or global routing. */
    uint32_t rkey;         /* Memory-region key authorizing RDMA WRITE access. */
    uint64_t addr;         /* Virtual address of the peer's registered buffer. */
};

/* Owns the RDMA resources and configuration used by the benchmark. */
struct rdma_ctx {
    struct ibv_context *ctx;     /* Open RDMA device context. */
    struct ibv_pd      *pd;      /* Protection domain owning RDMA resources. */
    struct ibv_mr      *mr;      /* Registered local buffer memory region. */
    struct ibv_cq      *cq;      /* Completion queue for RDMA work. */
    struct ibv_qp      *qp;      /* Reliable-connected queue pair. */
    char               *buf;     /* Aligned buffer used by RDMA WRITE operations. */
    int                 size;    /* Write size and registered-buffer size in bytes. */
    int                 ib_port; /* RDMA device port used by the QP. */
    int                 gid_idx; /* GID table index, or -1 for LID mode. */
    union ibv_gid       gid;     /* Selected local GID for global routing. */
};

/* ── helpers ─────────────────────────────────────────────────────────── */

/* Returns monotonic elapsed time in microseconds. */
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}

/* ── RDMA context lifecycle ──────────────────────────────────────────── */

/* Allocates and initializes the RDMA device, memory, CQ, and QP. */
static struct rdma_ctx *ctx_create(const char *dev_name, int size,
                                   int ib_port, int gid_idx,
                                   int mr_flags)
{
    struct rdma_ctx    *c;
    struct ibv_device **list;
    struct ibv_device  *dev = NULL;
    int                 n, i;

    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->size    = size;
    c->ib_port = ib_port;
    c->gid_idx = gid_idx;

    if (posix_memalign((void **)&c->buf, 4096, size)) {
        free(c);
        return NULL;
    }
    memset(c->buf, 0xAB, size);

    /* ── open device ── */
    list = ibv_get_device_list(&n);
    if (!list || n == 0) {
        fprintf(stderr, "No RDMA devices found. Is the VF visible?\n");
        goto err_buf;
    }

    for (i = 0; i < n; i++) {
        printf("  Found device: %s\n", ibv_get_device_name(list[i]));
        if (!dev_name || !strcmp(ibv_get_device_name(list[i]), dev_name))
            if (!dev) dev = list[i];
    }

    if (!dev) {
        fprintf(stderr, "Device '%s' not found\n", dev_name ? dev_name : "(any)");
        ibv_free_device_list(list);
        goto err_buf;
    }

    printf("Using device: %s\n", ibv_get_device_name(dev));
    c->ctx = ibv_open_device(dev);
    ibv_free_device_list(list);
    if (!c->ctx) { perror("ibv_open_device"); goto err_buf; }

    /* ── protection domain ── */
    c->pd = ibv_alloc_pd(c->ctx);
    if (!c->pd) { perror("ibv_alloc_pd"); goto err_ctx; }

    /* ── register memory ── */
    c->mr = ibv_reg_mr(c->pd, c->buf, size, mr_flags);
    if (!c->mr) { perror("ibv_reg_mr"); goto err_pd; }

    /* ── completion queue ── */
    c->cq = ibv_create_cq(c->ctx, TX_DEPTH * 2, NULL, NULL, 0);
    if (!c->cq) { perror("ibv_create_cq"); goto err_mr; }

    /* ── queue pair ── */
    {
        struct ibv_qp_init_attr attr = {
            .send_cq = c->cq,
            .recv_cq = c->cq,
            .cap     = {
                .max_send_wr  = TX_DEPTH + 1,
                .max_recv_wr  = 2,
                .max_send_sge = 1,
                .max_recv_sge = 1,
            },
            .qp_type = IBV_QPT_RC,
        };
        c->qp = ibv_create_qp(c->pd, &attr);
        if (!c->qp) { perror("ibv_create_qp"); goto err_cq; }
    }

    /* ── QP → INIT ── */
    {
        struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_INIT,
            .pkey_index      = 0,
            .port_num        = (uint8_t)ib_port,
            .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                               IBV_ACCESS_REMOTE_WRITE |
                               IBV_ACCESS_REMOTE_READ,
        };
        if (ibv_modify_qp(c->qp, &attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                          IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS)) {
            perror("ibv_modify_qp → INIT");
            goto err_qp;
        }
    }

    /* ── auto-detect RoCE: if LID==0 the port is Ethernet, GID is required ── */
    {
        struct ibv_port_attr pa;
        if (ibv_query_port(c->ctx, ib_port, &pa) == 0 &&
            pa.link_layer == IBV_LINK_LAYER_ETHERNET && c->gid_idx < 0) {
            printf("NOTE: RoCE/Ethernet port detected (LID=0). "
                   "Auto-selecting GID index 0.\n"
                   "      Use -g <idx> to override. "
                   "List GIDs with: ibv_devinfo -d %s -v | grep GID\n",
                   ibv_get_device_name(c->ctx->device));
            c->gid_idx = 0;
        }
    }

    /* ── query GID (required for RoCE) ── */
    if (c->gid_idx >= 0) {
        if (ibv_query_gid(c->ctx, ib_port, c->gid_idx, &c->gid)) {
            fprintf(stderr, "ibv_query_gid(idx=%d) failed\n", c->gid_idx);
            goto err_qp;
        }
    }

    return c;

err_qp:  ibv_destroy_qp(c->qp);
err_cq:  ibv_destroy_cq(c->cq);
err_mr:  ibv_dereg_mr(c->mr);
err_pd:  ibv_dealloc_pd(c->pd);
err_ctx: ibv_close_device(c->ctx);
err_buf: free(c->buf); free(c);
    return NULL;
}

/* Releases all RDMA resources owned by a context. */
static void ctx_destroy(struct rdma_ctx *c)
{
    ibv_destroy_qp(c->qp);
    ibv_destroy_cq(c->cq);
    ibv_dereg_mr(c->mr);
    ibv_dealloc_pd(c->pd);
    ibv_close_device(c->ctx);
    free(c->buf);
    free(c);
}

/* Collects local QP and memory-region metadata for TCP exchange. */
static void ctx_get_peer_info(struct rdma_ctx *c, struct peer_info *p)
{
    struct ibv_port_attr pa;
    memset(p, 0, sizeof(*p));
    ibv_query_port(c->ctx, c->ib_port, &pa);
    p->lid  = pa.lid;
    p->qpn  = c->qp->qp_num;
    p->psn  = BASE_PSN;
    p->rkey = c->mr->rkey;
    p->addr = (uint64_t)(uintptr_t)c->buf;
    if (c->gid_idx >= 0)
        memcpy(p->gid, &c->gid, 16);
}

/* Transition QP from INIT → RTR → RTS using remote peer info */
/* Applies remote addressing information and connects the local QP. */
static int ctx_connect(struct rdma_ctx *c, const struct peer_info *rem)
{
    /* ── INIT → RTR ── */
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_1024,
        .dest_qp_num        = rem->qpn,
        .rq_psn             = rem->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr = {
            .is_global  = 0,
            .dlid       = rem->lid,
            .sl         = 0,
            .port_num   = (uint8_t)c->ib_port,
        },
    };

    if (c->gid_idx >= 0) {
        attr.ah_attr.is_global          = 1;
        attr.ah_attr.grh.hop_limit      = 1;
        attr.ah_attr.grh.sgid_index     = (uint8_t)c->gid_idx;
        memcpy(&attr.ah_attr.grh.dgid, rem->gid, 16);
    }

    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        perror("ibv_modify_qp → RTR");
        return -1;
    }

    /* ── RTR → RTS ── */
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = BASE_PSN;
    attr.max_rd_atomic = 16;
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("ibv_modify_qp → RTS");
        return -1;
    }

    return 0;
}

/* ── send/recv/write helpers ─────────────────────────────────────────── */

/* Posts one RDMA WRITE operation using the local registered buffer. */
static int post_write(struct rdma_ctx *c, uint64_t remote_addr,
                      uint32_t rkey, int send_flags)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)c->buf,
        .length = (uint32_t)c->size,
        .lkey   = c->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = 0,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = send_flags,
        .wr.rdma    = {
            .remote_addr = remote_addr,
            .rkey        = rkey,
        },
    };
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(c->qp, &wr, &bad);
}

/* Posts a receive work request for the completion notification. */
static int post_recv(struct rdma_ctx *c)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)c->buf,
        .length = (uint32_t)c->size,
        .lkey   = c->mr->lkey,
    };
    struct ibv_recv_wr wr  = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(c->qp, &wr, &bad);
}

/* Posts a signaled send work request for data or completion notification. */
static int post_send(struct rdma_ctx *c)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)c->buf,
        .length = (uint32_t)c->size,
        .lkey   = c->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = 1,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
    };
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(c->qp, &wr, &bad);
}

/* Spin-poll the CQ until one completion arrives */
/* Waits for one successful completion queue entry. */
static int poll_one(struct rdma_ctx *c)
{
    struct ibv_wc wc;
    int ne;
    do { ne = ibv_poll_cq(c->cq, 1, &wc); } while (ne == 0);
    if (ne < 0) {
        fprintf(stderr, "ibv_poll_cq error\n");
        return -1;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "WC error (wr_id=%llu): %s\n",
                (unsigned long long)wc.wr_id,
                ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

/* ── bandwidth measurement ───────────────────────────────────────────── */

/* Runs the pipelined RDMA WRITE workload and prints bandwidth results. */
static int run_bw_client(struct rdma_ctx *c, uint64_t raddr,
                         uint32_t rkey, int iters, int size)
{
    struct ibv_wc wc[16];
    int scnt    = 0;   /* total WRs posted */
    int ccnt    = 0;   /* total completions reaped */
    int pending = 0;   /* WRs in flight (posted but not completed) */
    int poll_cnt = 0;  /* number of signaled WRs posted */
    double t_start, t_end;

    t_start = now_us();

    while (ccnt < iters) {
        /* ── post writes while pipeline has room ── */
        while (scnt < iters && pending < TX_DEPTH) {
            int last       = (scnt == iters - 1);
            int do_signal  = last || ((scnt % CQ_MOD) == (CQ_MOD - 1));
            int flags      = do_signal ? IBV_SEND_SIGNALED : 0;

            if (post_write(c, raddr, rkey, flags)) {
                fprintf(stderr, "post_write failed at scnt=%d\n", scnt);
                return -1;
            }
            if (do_signal)
                poll_cnt++;
            scnt++;
            pending++;
        }

        /* ── poll for completions ── */
        if (poll_cnt > 0) {
            int ne = ibv_poll_cq(c->cq, 16, wc);
            if (ne < 0) {
                fprintf(stderr, "ibv_poll_cq error\n");
                return -1;
            }
            for (int i = 0; i < ne; i++) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "WC error (wr_id=%llu): %s\n",
                            (unsigned long long)wc[i].wr_id,
                            ibv_wc_status_str(wc[i].status));
                    return -1;
                }
                /* each completion accounts for up to CQ_MOD writes */
                int acked = (pending < CQ_MOD) ? pending : CQ_MOD;
                pending  -= acked;
                ccnt     += acked;
            }
            poll_cnt -= ne;
        }
    }

    t_end = now_us();

    double elapsed_us = t_end - t_start;
    double elapsed_s  = elapsed_us * 1e-6;
    double bytes      = (double)iters * size;
    double gbps       = (bytes * 8.0) / (elapsed_s * 1e9);
    double mpps       = iters / (elapsed_s * 1e6);

    printf("\n=== Results ===\n");
    printf("  Iters:    %d\n",    iters);
    printf("  Size:     %d bytes\n", size);
    printf("  Elapsed:  %.2f us\n",  elapsed_us);
    printf("  BW:       %.3f Gbps\n", gbps);
    printf("  Rate:     %.3f Mpps\n", mpps);

    return 0;
}

/* ── TCP out-of-band helpers ─────────────────────────────────────────── */

/* Listens for and accepts one TCP client connection. */
static int tcp_listen_accept(int tcp_port)
{
    int srv, cli, opt = 1;
    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(tcp_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    socklen_t len = sizeof(sa);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return -1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa))) { perror("bind"); close(srv); return -1; }
    if (listen(srv, 1))                                { perror("listen"); close(srv); return -1; }

    printf("Waiting for client on TCP port %d...\n", tcp_port);
    cli = accept(srv, (struct sockaddr *)&sa, &len);
    close(srv);
    if (cli < 0) { perror("accept"); return -1; }
    return cli;
}

/* Connects to the benchmark peer over TCP. */
static int tcp_connect_to(const char *host, int tcp_port)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_s[8];
    int fd;

    snprintf(port_s, sizeof(port_s), "%d", tcp_port);
    if (getaddrinfo(host, port_s, &hints, &res)) {
        fprintf(stderr, "getaddrinfo('%s') failed\n", host);
        return -1;
    }
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) {
        perror("connect");
        freeaddrinfo(res);
        close(fd);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Bidirectional exchange of peer_info structs */
/* Exchanges local and remote peer metadata in the requested order. */
static int xchg_peer(int fd, const struct peer_info *local,
                     struct peer_info *remote, int send_first)
{
    ssize_t n = sizeof(*local);
    if (send_first) {
        if (send(fd, local,  n, 0)          != n) return -1;
        if (recv(fd, remote, n, MSG_WAITALL) != n) return -1;
    } else {
        if (recv(fd, remote, n, MSG_WAITALL) != n) return -1;
        if (send(fd, local,  n, 0)          != n) return -1;
    }
    return 0;
}

/* ── display helpers ─────────────────────────────────────────────────── */

/* Prints a 16-byte GID in colon-separated hexadecimal form. */
static void print_gid(const uint8_t *g)
{
    printf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
           "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
           g[0],g[1],g[2],g[3],g[4],g[5],g[6],g[7],
           g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);
}

/* Prints peer QP and memory-region metadata for diagnostics. */
static void print_dest(const char *label, const struct peer_info *p, int show_gid)
{
    printf("  %-6s QPN=0x%06x  LID=0x%04x  PSN=0x%06x"
           "  rkey=0x%08x  addr=0x%016llx",
           label, p->qpn, p->lid, p->psn, p->rkey,
           (unsigned long long)p->addr);
    if (show_gid) { printf("  GID="); print_gid(p->gid); }
    printf("\n");
}

/* ── usage ───────────────────────────────────────────────────────────── */

/* Prints command-line usage and option descriptions. */
static void usage(const char *prog)
{
    printf(
        "RDMA Write Bandwidth Benchmark (Mellanox ConnectX-4 / VF)\n\n"
        "Usage:\n"
        "  Server: %s [options]\n"
        "  Client: %s [options] <server-ip-or-hostname>\n\n"
        "Options:\n"
        "  -d <dev>    RDMA device name (default: first found)\n"
        "              Example: -d mlx5_0\n"
        "  -i <port>   InfiniBand port number (default: 1)\n"
        "  -p <port>   TCP port for connection setup (default: 18516)\n"
        "  -n <iters>  Number of WRITE iterations (default: 5000)\n"
        "  -s <size>   Message size in bytes (default: 65536)\n"
        "  -g <idx>    GID index for RoCE/Ethernet mode\n"
        "              -1 = InfiniBand LID mode (default)\n"
        "              Use: ibv_devinfo -v | grep GID  to find valid indices\n"
        "  -h          Show this help\n\n"
        "Quick start (IB mode):\n"
        "  VM1: %s -d mlx5_0\n"
        "  VM2: %s -d mlx5_0 <VM1-IP>\n\n"
        "Quick start (RoCE mode):\n"
        "  VM1: %s -d mlx5_0 -g 3\n"
        "  VM2: %s -d mlx5_0 -g 3 <VM1-IP>\n",
        prog, prog, prog, prog, prog, prog);
}

/* ── main ────────────────────────────────────────────────────────────── */

/* Runs the passive server or active RDMA WRITE bandwidth benchmark. */
int main(int argc, char *argv[])
{
    const char *dev_name = NULL;
    const char *server   = NULL;
    int tcp_port = DEFAULT_TCP_PORT;
    int iters    = DEFAULT_ITERS;
    int size     = DEFAULT_MSG_SIZE;
    int ib_port  = DEFAULT_IB_PORT;
    int gid_idx  = DEFAULT_GID_IDX;
    int opt;

    while ((opt = getopt(argc, argv, "d:i:p:n:s:g:h")) != -1) {
        switch (opt) {
        case 'd': dev_name = optarg;        break;
        case 'i': ib_port  = atoi(optarg);  break;
        case 'p': tcp_port = atoi(optarg);  break;
        case 'n': iters    = atoi(optarg);  break;
        case 's': size     = atoi(optarg);  break;
        case 'g': gid_idx  = atoi(optarg);  break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    server = (optind < argc) ? argv[optind] : NULL;
    int is_server = (server == NULL);

    printf("\n=== RDMA Write BW ===\n");
    printf("Mode:     %s\n", is_server ? "server (responder)" : "client (initiator)");
    printf("Size:     %d bytes\n", size);
    printf("Iters:    %d\n", iters);
    printf("IB port:  %d\n", ib_port);
    if (gid_idx < 0)
        printf("GID idx:  auto-detect\n\n");
    else
        printf("GID idx:  %d (RoCE mode)\n\n", gid_idx);

    /* ── create RDMA context ── */
    int mr_flags = is_server
        ? (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE)
        : IBV_ACCESS_LOCAL_WRITE;

    struct rdma_ctx *rctx = ctx_create(dev_name, size, ib_port, gid_idx, mr_flags);
    if (!rctx) {
        fprintf(stderr, "Failed to create RDMA context\n");
        return 1;
    }

    /* ── get local peer info ── */
    struct peer_info local, remote;
    ctx_get_peer_info(rctx, &local);
    print_dest("local", &local, rctx->gid_idx >= 0);

    /* ── TCP exchange of peer info ── */
    int sock;
    if (is_server) {
        /* post_recv before TCP exchange so it is ready before client writes */
        if (post_recv(rctx)) {
            fprintf(stderr, "post_recv failed\n");
            ctx_destroy(rctx);
            return 1;
        }

        sock = tcp_listen_accept(tcp_port);
        if (sock < 0) { ctx_destroy(rctx); return 1; }
        if (xchg_peer(sock, &local, &remote, 0)) {
            fprintf(stderr, "peer info exchange failed\n");
            close(sock); ctx_destroy(rctx); return 1;
        }
    } else {
        printf("Connecting to %s:%d...\n", server, tcp_port);
        sock = tcp_connect_to(server, tcp_port);
        if (sock < 0) { ctx_destroy(rctx); return 1; }
        if (xchg_peer(sock, &local, &remote, 1)) {
            fprintf(stderr, "peer info exchange failed\n");
            close(sock); ctx_destroy(rctx); return 1;
        }
    }
    close(sock);
    print_dest("remote", &remote, rctx->gid_idx >= 0);

    /* ── connect QPs ── */
    if (ctx_connect(rctx, &remote)) {
        fprintf(stderr, "QP connection failed\n");
        ctx_destroy(rctx);
        return 1;
    }
    printf("\nQP connected (INIT → RTR → RTS).\n\n");

    /* ── data path ── */
    int rc = 0;

    if (is_server) {
        printf("Server ready, waiting for client to finish writes...\n");
        if (poll_one(rctx)) {   /* wait for "done" SEND from client */
            rc = 1;
        } else {
            printf("Server done.\n");
        }
    } else {
        printf("Starting RDMA WRITE bandwidth test...\n");
        if (run_bw_client(rctx, remote.addr, remote.rkey, iters, size)) {
            rc = 1;
        } else {
            /* notify server that all writes are complete */
            if (post_send(rctx)) {
                fprintf(stderr, "post_send (done signal) failed\n");
                rc = 1;
            } else if (poll_one(rctx)) {   /* wait for send completion */
                rc = 1;
            }
        }
    }

    ctx_destroy(rctx);
    return rc;
}
