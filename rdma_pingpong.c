/*
 * rdma_pingpong.c - RDMA Send/Receive ping-pong latency benchmark
 *
 * Uses RC (Reliable Connected) QPs with IBV_WR_SEND/RECV operations.
 * QP info is exchanged out-of-band over a TCP socket.
 *
 * Usage:
 *   Server (VM1): ./rdma_pingpong [options]
 *   Client (VM2): ./rdma_pingpong [options] <server-ip>
 *
 * Example (IB mode):
 *   VM1: ./rdma_pingpong -d mlx5_0 -i 1
 *   VM2: ./rdma_pingpong -d mlx5_0 -i 1 192.168.1.10
 *
 * Example (RoCE mode, GID index required):
 *   VM1: ./rdma_pingpong -d mlx5_0 -i 1 -g 3
 *   VM2: ./rdma_pingpong -d mlx5_0 -i 1 -g 3 192.168.1.10
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
#define DEFAULT_TCP_PORT  18515
#define DEFAULT_ITERS     1000
#define DEFAULT_MSG_SIZE  64
#define DEFAULT_IB_PORT   1
#define DEFAULT_GID_IDX   -1    /* -1 = IB/LID mode; ≥0 = RoCE/GID mode */
#define CQ_DEPTH          16
#define BASE_PSN          0x123456

/* ── data structures ─────────────────────────────────────────────────── */

/* Exchanged over TCP to set up the RDMA connection */
struct qp_dest {
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    uint8_t  gid[16];
};

struct rdma_ctx {
    struct ibv_context *ctx;
    struct ibv_pd      *pd;
    struct ibv_mr      *mr;
    struct ibv_cq      *cq;
    struct ibv_qp      *qp;
    char               *buf;
    int                 size;
    int                 ib_port;
    int                 gid_idx;
    union ibv_gid       gid;
};

/* ── helpers ─────────────────────────────────────────────────────────── */

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* ── RDMA context lifecycle ──────────────────────────────────────────── */

static struct rdma_ctx *ctx_create(const char *dev_name, int size,
                                   int ib_port, int gid_idx)
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
    memset(c->buf, 0xAB, size);   /* fill with pattern */

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
    c->mr = ibv_reg_mr(c->pd, c->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!c->mr) { perror("ibv_reg_mr"); goto err_pd; }

    /* ── completion queue ── */
    c->cq = ibv_create_cq(c->ctx, CQ_DEPTH, NULL, NULL, 0);
    if (!c->cq) { perror("ibv_create_cq"); goto err_mr; }

    /* ── queue pair ── */
    {
        struct ibv_qp_init_attr attr = {
            .send_cq = c->cq,
            .recv_cq = c->cq,
            .cap     = {
                .max_send_wr  = 1,
                .max_recv_wr  = 1,
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
            .qp_access_flags = IBV_ACCESS_LOCAL_WRITE,
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

static void ctx_get_dest(struct rdma_ctx *c, struct qp_dest *d)
{
    struct ibv_port_attr pa;
    memset(d, 0, sizeof(*d));
    ibv_query_port(c->ctx, c->ib_port, &pa);
    d->lid = pa.lid;
    d->qpn = c->qp->qp_num;
    d->psn = BASE_PSN;
    if (c->gid_idx >= 0)
        memcpy(d->gid, &c->gid, 16);
}

/* Transition QP from INIT → RTR → RTS using remote dest info */
static int ctx_connect(struct rdma_ctx *c, const struct qp_dest *rem)
{
    /* ── INIT → RTR ── */
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_1024,
        .dest_qp_num        = rem->qpn,
        .rq_psn             = rem->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,   /* ~0.655 ms RNR retry timer */
        .ah_attr = {
            .is_global  = 0,
            .dlid       = rem->lid,
            .sl         = 0,
            .port_num   = (uint8_t)c->ib_port,
        },
    };

    if (c->gid_idx >= 0) {
        /* RoCE / loopback-over-Ethernet: must use GID routing */
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
    attr.timeout       = 14;    /* ~67 ms ACK timeout */
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;     /* infinite RNR retries */
    attr.sq_psn        = BASE_PSN;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("ibv_modify_qp → RTS");
        return -1;
    }

    return 0;
}

/* ── send/recv helpers ───────────────────────────────────────────────── */

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

/* ── TCP out-of-band helpers ─────────────────────────────────────────── */

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

/* Bidirectional exchange of qp_dest structs */
static int xchg_dest(int fd, const struct qp_dest *local,
                     struct qp_dest *remote, int send_first)
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

static void print_gid(const uint8_t *g)
{
    printf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
           "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
           g[0],g[1],g[2],g[3],g[4],g[5],g[6],g[7],
           g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);
}

static void print_dest(const char *label, const struct qp_dest *d, int show_gid)
{
    printf("  %-6s QPN=0x%06x  LID=0x%04x  PSN=0x%06x",
           label, d->qpn, d->lid, d->psn);
    if (show_gid) { printf("  GID="); print_gid(d->gid); }
    printf("\n");
}

/* ── usage ───────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    printf(
        "RDMA Ping-Pong Latency Benchmark (Mellanox ConnectX-4 / VF)\n\n"
        "Usage:\n"
        "  Server: %s [options]\n"
        "  Client: %s [options] <server-ip-or-hostname>\n\n"
        "Options:\n"
        "  -d <dev>    RDMA device name (default: first found)\n"
        "              Example: -d mlx5_0\n"
        "  -i <port>   InfiniBand port number (default: 1)\n"
        "  -p <port>   TCP port for connection setup (default: 18515)\n"
        "  -n <iters>  Number of ping-pong iterations (default: 1000)\n"
        "  -s <size>   Message size in bytes (default: 64)\n"
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

    printf("\n=== RDMA Ping-Pong ===\n");
    printf("Mode:     %s\n", is_server ? "server (responder)" : "client (initiator)");
    printf("Size:     %d bytes\n", size);
    printf("Iters:    %d\n", iters);
    printf("IB port:  %d\n", ib_port);
    if (gid_idx < 0)
        printf("GID idx:  auto-detect\n\n");
    else
        printf("GID idx:  %d (RoCE mode)\n\n", gid_idx);

    /* ── create RDMA context ── */
    struct rdma_ctx *rctx = ctx_create(dev_name, size, ib_port, gid_idx);
    if (!rctx) {
        fprintf(stderr, "Failed to create RDMA context\n");
        return 1;
    }

    /* ── get local QP info ── */
    struct qp_dest local, remote;
    ctx_get_dest(rctx, &local);
    print_dest("local", &local, rctx->gid_idx >= 0);

    /* ── TCP exchange of QP info ── */
    int sock;
    if (is_server) {
        sock = tcp_listen_accept(tcp_port);
        if (sock < 0) { ctx_destroy(rctx); return 1; }
        /* server: receive client info first, then send ours */
        if (xchg_dest(sock, &local, &remote, 0)) {
            fprintf(stderr, "QP info exchange failed\n");
            close(sock); ctx_destroy(rctx); return 1;
        }
    } else {
        printf("Connecting to %s:%d...\n", server, tcp_port);
        sock = tcp_connect_to(server, tcp_port);
        if (sock < 0) { ctx_destroy(rctx); return 1; }
        /* client: send our info first, then receive server's */
        if (xchg_dest(sock, &local, &remote, 1)) {
            fprintf(stderr, "QP info exchange failed\n");
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
    printf("\nQP connected (INIT → RTR → RTS). Starting ping-pong...\n\n");

    /* ── ping-pong loop ── */
    double *lat = malloc(iters * sizeof(double));
    if (!lat) { ctx_destroy(rctx); return 1; }

    int rc = 0;

    if (is_server) {
        /*
         * Server (responder):
         *   post_recv → poll (ping arrives) → post_send → poll (pong sent)
         */
        for (int i = 0; i < iters && !rc; i++) {
            if (post_recv(rctx))  { fprintf(stderr, "post_recv failed\n"); rc = 1; break; }
            if (poll_one(rctx))   { rc = 1; break; }  /* ping received */
            if (post_send(rctx))  { fprintf(stderr, "post_send failed\n"); rc = 1; break; }
            if (poll_one(rctx))   { rc = 1; break; }  /* pong sent */

            if ((i + 1) % 100 == 0)
                printf("  server: completed %d / %d\r", i + 1, iters);
        }
        printf("\nServer done: %d exchanges %s\n",
               iters, rc ? "(ERROR)" : "OK");

    } else {
        /*
         * Client (initiator):
         *   post_recv first (ready for pong) → start timer → post_send
         *   → poll send done → poll recv done → stop timer
         *
         * Timing includes the full round trip from send to pong received.
         */
        for (int i = 0; i < iters && !rc; i++) {
            if (post_recv(rctx)) { fprintf(stderr, "post_recv failed\n"); rc = 1; break; }

            double t = now_us();
            if (post_send(rctx)) { fprintf(stderr, "post_send failed\n"); rc = 1; break; }
            if (poll_one(rctx))  { rc = 1; break; }   /* local send done */
            if (poll_one(rctx))  { rc = 1; break; }   /* pong received */
            lat[i] = now_us() - t;
        }

        if (!rc) {
            /* ── compute statistics ── */
            double sum = 0, mn = lat[0], mx = lat[0];
            for (int i = 0; i < iters; i++) {
                sum += lat[i];
                if (lat[i] < mn) mn = lat[i];
                if (lat[i] > mx) mx = lat[i];
            }
            double avg = sum / iters;

            double *s = malloc(iters * sizeof(double));
            memcpy(s, lat, iters * sizeof(double));
            qsort(s, iters, sizeof(double), cmp_double);

            double p50  = s[iters / 2];
            double p99  = s[(int)(iters * 0.99)];
            double p999 = s[(int)(iters * 0.999)];
            free(s);

            printf("=== Results (%d iters, %d bytes/msg) ===\n", iters, size);
            printf("  RTT latency (us):\n");
            printf("    min      : %8.2f\n", mn);
            printf("    avg      : %8.2f\n", avg);
            printf("    median   : %8.2f\n", p50);
            printf("    p99      : %8.2f\n", p99);
            printf("    p99.9    : %8.2f\n", p999);
            printf("    max      : %8.2f\n", mx);
            printf("  One-way (avg): %.2f us\n", avg / 2.0);
            printf("  Throughput:    %.0f msg/s\n", 1e6 / avg);
        }
    }

    free(lat);
    ctx_destroy(rctx);
    return rc;
}
