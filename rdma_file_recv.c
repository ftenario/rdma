#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define PORT        18515
#define IB_PORT     1
#define GID_INDEX   3
#define CHUNK_SIZE  (256UL * 1024 * 1024)

/* Connection and memory-region metadata exchanged during the RDMA handshake. */
struct conn_info {
    uint32_t qp_num;       /* Remote queue pair number used to address the QP. */
    uint16_t lid;          /* Local identifier used for InfiniBand routing. */
    uint8_t  gid[16];      /* Global identifier used for RoCE or global routing. */
    uint32_t rkey;         /* Memory-region key authorizing remote RDMA access. */
    uint64_t remote_addr;  /* Virtual address of the registered transfer buffer. */
    uint64_t file_size;    /* Total file size in bytes for transfer coordination. */
};

/* Describes the file offset and length of one received chunk. */
struct chunk_msg {
    uint64_t offset;       /* File offset where this chunk begins. */
    uint32_t length;       /* Number of bytes in this chunk. */
};

/* Sends exactly len bytes over the TCP control connection. */
static ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

/* Receives exactly len bytes from the TCP control connection. */
static ssize_t recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* Parses arguments, establishes RDMA, and writes received chunks to disk. */
int main(int argc, char *argv[]) {
    int verbose = 0;
    int summary = 0;
    int quiet = 0;

    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "--verbose") == 0 || strcmp(argv[argi], "-v") == 0) {
            verbose = 1;
            argi++;
            continue;
        }
        if (strcmp(argv[argi], "--summary") == 0 || strcmp(argv[argi], "-s") == 0) {
            summary = 1;
            argi++;
            continue;
        }
        if (strcmp(argv[argi], "--quiet") == 0 || strcmp(argv[argi], "-q") == 0) {
            quiet = 1;
            argi++;
            continue;
        }
        break;
    }

    if (argc - argi < 1) {
        printf("Usage: %s [--verbose|--summary|--quiet] <output_file>\n", argv[0]);
        return 1;
    }

    const char *out_filename = argv[argi];

    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return 1;
    }

    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx) {
        perror("ibv_open_device");
        return 1;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        return 1;
    }

    char *recv_buf = malloc(CHUNK_SIZE);
    if (!recv_buf) {
        perror("malloc");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_mr *mr = ibv_reg_mr(pd, recv_buf, CHUNK_SIZE,
                                  IBV_ACCESS_LOCAL_WRITE |
                                  IBV_ACCESS_REMOTE_WRITE |
                                  IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        perror("ibv_reg_mr");
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq");
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 16,
            .max_recv_wr = 16,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) {
        perror("ibv_create_qp");
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    if (listen(server_fd, 1) != 0) {
        perror("listen");
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    if (!quiet) {
        printf("Waiting for sender to connect...\n");
    }
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct conn_info remote;
    if (recv_all(client_fd, &remote, sizeof(remote)) < 0) {
        perror("recv remote info");
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    uint64_t file_size = remote.file_size;
    if (file_size == 0) {
        fprintf(stderr, "Remote file size is zero\n");
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    int out_fd = open(out_filename, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (out_fd < 0) {
        perror("open");
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    if (ftruncate(out_fd, (off_t)file_size) != 0) {
        perror("ftruncate");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, IB_PORT, &port_attr) != 0) {
        perror("ibv_query_port");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    union ibv_gid local_gid;
    if (ibv_query_gid(ctx, IB_PORT, GID_INDEX, &local_gid) != 0) {
        perror("ibv_query_gid");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct conn_info local = {
        .qp_num      = qp->qp_num,
        .lid         = port_attr.lid,
        .rkey        = mr->rkey,
        .remote_addr = (uint64_t)(uintptr_t)recv_buf,
        .file_size   = file_size,
    };
    memcpy(local.gid, &local_gid, 16);
    if (send_all(client_fd, &local, sizeof(local)) < 0) {
        perror("send local info");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_qp_attr init_attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = IB_PORT,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
    };
    if (ibv_modify_qp(qp, &init_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                     IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("ibv_modify_qp INIT");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    union ibv_gid remote_gid;
    memcpy(&remote_gid, remote.gid, 16);
    struct ibv_qp_attr rtr_attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = IBV_MTU_1024,
        .dest_qp_num        = remote.qp_num,
        .rq_psn             = 0,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr = {
            .is_global  = 1,
            .dlid       = remote.lid,
            .sl         = 0,
            .port_num   = IB_PORT,
            .grh = {
                .dgid       = remote_gid,
                .sgid_index = GID_INDEX,
                .hop_limit  = 1,
            },
        },
    };
    if (ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE | IBV_QP_AV |
                                    IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                                    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                                    IBV_QP_MIN_RNR_TIMER)) {
        perror("ibv_modify_qp RTR");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    struct ibv_qp_attr rts_attr = {
        .qp_state      = IBV_QPS_RTS,
        .timeout       = 14,
        .retry_cnt     = 7,
        .rnr_retry     = 7,
        .sq_psn        = 0,
        .max_rd_atomic = 1,
    };
    if (ibv_modify_qp(qp, &rts_attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
                                    IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                    IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("ibv_modify_qp RTS");
        close(out_fd);
        close(client_fd);
        close(server_fd);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(recv_buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    if (!quiet) {
        printf("RDMA connection ready! Waiting for data...\n");
    }

    uint64_t received = 0;
    while (received < file_size) {
        struct chunk_msg msg;
        if (recv_all(client_fd, &msg, sizeof(msg)) < 0) {
            perror("recv chunk metadata");
            break;
        }

        if (msg.length == 0 || msg.length > CHUNK_SIZE) {
            fprintf(stderr, "Invalid chunk length %u at offset %llu\n",
                    msg.length,
                    (unsigned long long)msg.offset);
            break;
        }

        if (msg.offset != received) {
            fprintf(stderr, "Unexpected offset %llu, expected %llu\n",
                    (unsigned long long)msg.offset,
                    (unsigned long long)received);
            break;
        }

        int ready = 0;
        if (recv_all(client_fd, &ready, sizeof(ready)) < 0) {
            perror("recv data ready");
            break;
        }
        if (ready != 1) {
            fprintf(stderr, "Invalid ready flag for offset %llu\n",
                    (unsigned long long)msg.offset);
            break;
        }

        ssize_t written = pwrite(out_fd, recv_buf, msg.length, (off_t)msg.offset);
        if (written != (ssize_t)msg.length) {
            perror("pwrite");
            break;
        }

        int ack = 1;
        if (send_all(client_fd, &ack, sizeof(ack)) < 0) {
            perror("send ack");
            break;
        }

        received += msg.length;
        if (!quiet && verbose) {
            double percent = 100.0 * (double)received / (double)file_size;
            printf("Received %llu/%llu bytes (%.2f%%)\n",
                   (unsigned long long)received,
                   (unsigned long long)file_size,
                   percent);
        }
    }

    if (!quiet) {
        if (summary || verbose) {
            printf("Transferred %llu/%llu bytes (100.00%%)\n",
                   (unsigned long long)received,
                   (unsigned long long)file_size);
        }
        printf("Transfer complete: %llu bytes written to %s\n",
               (unsigned long long)received,
               out_filename);
    }

    close(out_fd);
    close(client_fd);
    close(server_fd);

    if (ibv_destroy_qp(qp) != 0) perror("ibv_destroy_qp");
    if (ibv_destroy_cq(cq) != 0) perror("ibv_destroy_cq");
    if (ibv_dereg_mr(mr) != 0) perror("ibv_dereg_mr");
    if (ibv_dealloc_pd(pd) != 0) perror("ibv_dealloc_pd");
    if (ibv_close_device(ctx) != 0) perror("ibv_close_device");
    free(recv_buf);

    if (!quiet) {
        printf("File received and saved to: %s ✅\n", out_filename);
    }
    return 0;
}
