/*
 * rdma_read_lat.c - RDMA Read one-sided latency benchmark
 *
 * Client issues RDMA READ operations directly into server memory.
 * The server CPU is completely idle during the measurement — no
 * recv WRs, no polling, no involvement whatsoever after setup.
 *
 * Usage:
 *   Server: ./rdma_read_lat [options]
 *   Client: ./rdma_read_lat [options] <server-ip>
 *
 * Example:
 *   VM1: ./rdma_read_lat -d rocep7s0 -g 3
 *   VM2: ./rdma_read_lat -d rocep7s0 -g 3 192.168.100.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <infiniband/verbs.h>

/* ── defaults ────────────────────────────────────────────────────────── */
#define DEFAULT_TCP_PORT  18517
#define DEFAULT_ITERS     1000
#define DEFAULT_SIZE      64
#define DEFAULT_IB_PORT   1
#define DEFAULT_GID_IDX   -1
#define CQ_DEPTH          16
#define BASE_PSN          0x123456

/* ── data structures ─────────────────────────────────────────────────── */

/*
 * Exchanged over TCP. Server shares its MR rkey + buf address so the
 * client can issue one-sided RDMA READs without server involvement.
 */
struct peer_info {
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    uint8_t  gid[16];
    uint32_t rkey;
    uint64_t addr;
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

/* ── timing ──────────────────────────────────────────────────────────── */

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

/* ── RDMA context ────────────────────────────────────────────────────── */

static struct rdma_ctx *ctx_create(const char *dev_name, int size,
                                   int ib_port, int gid_idx, int mr_flags)
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

    if (posix_memalign((void **)&c->buf, 4096, size)) { free(c); return NULL; }
    memset(c->buf, 0xCD, size);

    list = ibv_get_device_list(&n);
    if (!list || n == 0) { fprintf(stderr, "No RDMA devices found\n"); goto err_buf; }

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

    c->pd = ibv_alloc_pd(c->ctx);
    if (!c->pd) { perror("ibv_alloc_pd"); goto err_ctx; }

    c->mr = ibv_reg_mr(c->pd, c->buf, size, mr_flags);
    if (!c->mr) { perror("ibv_reg_mr"); goto err_pd; }

    c->cq = ibv_create_cq(c->ctx, CQ_DEPTH, NULL, NULL, 0);
    if (!c->cq) { perror("ibv_create_cq"); goto err_mr; }

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

    {
        struct ibv_qp_attr attr = {
            .qp_state        = IBV_QPS_INIT,
            .pkey_index      = 0,
            .port_num        = (uint8_t)ib_port,
            .qp_access_flags = IBV_ACCESS_LOCAL_WRITE  |
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

    /* ── auto-detect RoCE ── */
    {
        struct ibv_port_attr pa;
        if (ibv_query_port(c->ctx, ib_port, &pa) == 0 &&
            pa.link_layer == IBV_LINK_LAYER_ETHERNET && c->gid_idx < 0) {
            printf("NOTE: RoCE/Ethernet port detected (LID=0). "
                   "Auto-selecting GID index 0.\n"
                   "      Use -g <idx> to override. "
                   "List GIDs: ibv_devinfo -d %s -v | grep GID\n",
                   ibv_get_device_name(c->ctx->device));
            c->gid_idx = 0;
        }
    }

    if (c->gid_idx >= 0 &&
        ibv_query_gid(c->ctx, ib_port, c->gid_idx, &c->gid)) {
        fprintf(stderr, "ibv_query_gid(idx=%d) failed\n", c->gid_idx);
        goto err_qp;
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

static int ctx_connect(struct rdma_ctx *c, const struct peer_info *rem)
{
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_1024,
        .dest_qp_num        = rem->qpn,
        .rq_psn             = rem->psn,
        .max_dest_rd_atomic = 16,
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

/* ── send/recv/read work requests ────────────────────────────────────── */

/*
 * RDMA READ: client pulls data from server's registered memory.
 * No recv WR needed on the server — the NIC handles it entirely.
 */
static int post_read(struct rdma_ctx *c, uint64_t raddr, uint32_t rkey)
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
        .opcode     = IBV_WR_RDMA_READ,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = raddr,
            .rkey        = rkey,
        },
    };
    struct ibv_send_wr *bad = NULL;
    return ibv_post_send(c->qp, &wr, &bad);
}

/* Simple SEND used to notify server the test is done */
static int post_send_done(struct rdma_ctx *c)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)c->buf,
        .length = 1,
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

static int post_recv_done(struct rdma_ctx *c)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)(uintptr_t)c->buf,
        .length = (uint32_t)c->size,
        .lkey   = c->mr->lkey,
    };
    struct ibv_recv_wr wr  = { .wr_id = 2, .sg_list = &sge, .num_sge = 1 };
    struct ibv_recv_wr *bad = NULL;
    return ibv_post_recv(c->qp, &wr, &bad);
}

static int poll_one(struct rdma_ctx *c)
{
    struct ibv_wc wc;
    int ne;
    do { ne = ibv_poll_cq(c->cq, 1, &wc); } while (ne == 0);
    if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
        if (ne > 0)
            fprintf(stderr, "WC error (wr_id=%llu): %s\n",
                    (unsigned long long)wc.wr_id,
                    ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

/* ── TCP helpers ─────────────────────────────────────────────────────── */

static int tcp_listen_accept(int port)
{
    int srv, cli, opt = 1;
    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    socklen_t len = sizeof(sa);
    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) || listen(srv, 1)) {
        perror("bind/listen"); close(srv); return -1;
    }
    printf("Waiting for client on TCP port %d...\n", port);
    cli = accept(srv, (struct sockaddr *)&sa, &len);
    close(srv);
    return cli;
}

static int tcp_connect_to(const char *host, int port)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_s[8];
    int fd;
    snprintf(port_s, sizeof(port_s), "%d", port);
    if (getaddrinfo(host, port_s, &hints, &res)) {
        fprintf(stderr, "getaddrinfo('%s') failed\n", host); return -1;
    }
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) {
        perror("connect"); freeaddrinfo(res); close(fd); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

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

/* ── display ─────────────────────────────────────────────────────────── */

static void print_gid(const uint8_t *g)
{
    printf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
           "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
           g[0],g[1],g[2],g[3],g[4],g[5],g[6],g[7],
           g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15]);
}

static void print_peer(const char *label, const struct peer_info *p, int show_gid)
{
    printf("  %-6s QPN=0x%06x  LID=0x%04x  rkey=0x%08x  addr=0x%016llx",
           label, p->qpn, p->lid, p->rkey, (unsigned long long)p->addr);
    if (show_gid) { printf("\n         GID="); print_gid(p->gid); }
    printf("\n");
}

/* ── usage ───────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    printf(
        "RDMA Read Latency Benchmark\n\n"
        "Usage:\n"
        "  Server: %s [options]\n"
        "  Client: %s [options] <server-ip>\n\n"
        "Options:\n"
        "  -d <dev>    RDMA device (default: first found)\n"
        "  -i <port>   IB port number (default: 1)\n"
        "  -p <port>   TCP port (default: 18517)\n"
        "  -n <iters>  Iterations (default: 1000)\n"
        "  -s <size>   Read size in bytes (default: 64)\n"
        "  -g <idx>    GID index for RoCE (-1 = auto, default: -1)\n"
        "  -h          Show help\n",
        prog, prog);
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *dev_name = NULL, *server = NULL;
    int tcp_port = DEFAULT_TCP_PORT;
    int iters    = DEFAULT_ITERS;
    int size     = DEFAULT_SIZE;
    int ib_port  = DEFAULT_IB_PORT;
    int gid_idx  = DEFAULT_GID_IDX;
    int opt;

    while ((opt = getopt(argc, argv, "d:i:p:n:s:g:h")) != -1) {
        switch (opt) {
        case 'd': dev_name = optarg;       break;
        case 'i': ib_port  = atoi(optarg); break;
        case 'p': tcp_port = atoi(optarg); break;
        case 'n': iters    = atoi(optarg); break;
        case 's': size     = atoi(optarg); break;
        case 'g': gid_idx  = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    server    = (optind < argc) ? argv[optind] : NULL;
    int is_server = (server == NULL);

    printf("\n=== RDMA Read Latency ===\n");
    printf("Mode:     %s\n", is_server ? "server (passive)" : "client (initiator)");
    printf("Size:     %d bytes\n", size);
    printf("Iters:    %d\n\n", iters);

    /*
     * Server MR must allow IBV_ACCESS_REMOTE_READ so the client's NIC
     * can DMA directly out of server memory. Server CPU does nothing
     * during the actual measurement.
     */
    int mr_flags = IBV_ACCESS_LOCAL_WRITE |
                   (is_server ? IBV_ACCESS_REMOTE_READ : 0);

    struct rdma_ctx *rctx = ctx_create(dev_name, size, ib_port, gid_idx, mr_flags);
    if (!rctx) { fprintf(stderr, "Failed to create RDMA context\n"); return 1; }

    struct peer_info local, remote;
    ctx_get_peer_info(rctx, &local);
    print_peer("local", &local, rctx->gid_idx >= 0);

    /* Server pre-posts the "done" recv before TCP exchange */
    if (is_server && post_recv_done(rctx)) {
        fprintf(stderr, "post_recv_done failed\n"); ctx_destroy(rctx); return 1;
    }

    int sock;
    if (is_server) {
        sock = tcp_listen_accept(tcp_port);
        if (sock < 0) { ctx_destroy(rctx); return 1; }
        if (xchg_peer(sock, &local, &remote, 0)) {
            fprintf(stderr, "peer exchange failed\n");
            close(sock); ctx_destroy(rctx); return 1;
        }
    } else {
        printf("Connecting to %s:%d...\n", server, tcp_port);
        sock = tcp_connect_to(server, tcp_port);
        if (sock < 0) { ctx_destroy(rctx); return 1; }
        if (xchg_peer(sock, &local, &remote, 1)) {
            fprintf(stderr, "peer exchange failed\n");
            close(sock); ctx_destroy(rctx); return 1;
        }
    }
    close(sock);
    print_peer("remote", &remote, rctx->gid_idx >= 0);

    if (ctx_connect(rctx, &remote)) {
        fprintf(stderr, "QP connection failed\n"); ctx_destroy(rctx); return 1;
    }
    printf("\nQP connected. Starting...\n\n");

    int rc = 0;

    if (is_server) {
        /*
         * Server: completely passive during measurement.
         * Just wait for the client's "done" SEND notification.
         */
        printf("Server memory exposed for RDMA Read.\n");
        printf("Waiting for client to finish...\n");
        if (poll_one(rctx)) rc = 1;  /* waits for done SEND from client */
        printf("Server done.\n");

    } else {
        /*
         * Client: issue RDMA READ operations one at a time.
         * Each READ fetches data from server's registered buffer
         * without any server CPU involvement.
         */
        double *lat = malloc(iters * sizeof(double));
        if (!lat) { ctx_destroy(rctx); return 1; }

        for (int i = 0; i < iters && !rc; i++) {
            double t = now_us();
            if (post_read(rctx, remote.addr, remote.rkey)) {
                fprintf(stderr, "post_read failed\n"); rc = 1; break;
            }
            if (poll_one(rctx)) { rc = 1; break; }
            lat[i] = now_us() - t;
        }

        /* Notify server we are done */
        if (!rc && post_send_done(rctx)) rc = 1;
        if (!rc && poll_one(rctx))       rc = 1;

        if (!rc) {
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

            printf("=== RDMA Read Latency Results (%d iters, %d bytes) ===\n",
                   iters, size);
            printf("  NOTE: server CPU was idle during this entire measurement.\n\n");
            printf("  Latency (us):\n");
            printf("    min      : %8.2f\n", mn);
            printf("    avg      : %8.2f\n", avg);
            printf("    median   : %8.2f\n", p50);
            printf("    p99      : %8.2f\n", p99);
            printf("    p99.9    : %8.2f\n", p999);
            printf("    max      : %8.2f\n", mx);
            printf("  Throughput : %.0f reads/s\n", 1e6 / avg);
        }
        free(lat);
    }

    ctx_destroy(rctx);
    return rc;
}
