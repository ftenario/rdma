#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define PORT        18515
#define IB_PORT     1
#define GID_INDEX   3
#define CHUNK_SIZE  (256UL * 1024 * 1024)

struct conn_info {
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
    uint32_t rkey;
    uint64_t remote_addr;
    uint64_t file_size;
};

struct chunk_msg {
    uint64_t offset;
    uint32_t length;
};

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

    if (argc - argi < 2) {
        printf("Usage: %s [--verbose|--summary|--quiet] <server_ip> <file_to_send>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[argi];
    const char *filename = argv[argi + 1];

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    struct stat st;
    if (stat(filename, &st) != 0) {
        perror("stat");
        fclose(fp);
        return 1;
    }

    uint64_t file_size = (uint64_t)st.st_size;
    char *file_buf = malloc(CHUNK_SIZE);
    if (!file_buf) {
        perror("malloc");
        fclose(fp);
        return 1;
    }

    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx) {
        perror("ibv_open_device");
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct ibv_mr *mr = ibv_reg_mr(pd, file_buf, CHUNK_SIZE,
                                  IBV_ACCESS_LOCAL_WRITE |
                                  IBV_ACCESS_REMOTE_WRITE |
                                  IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        perror("ibv_reg_mr");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq");
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
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
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, IB_PORT, &port_attr) != 0) {
        perror("ibv_query_port");
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    union ibv_gid local_gid;
    if (ibv_query_gid(ctx, IB_PORT, GID_INDEX, &local_gid) != 0) {
        perror("ibv_query_gid");
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct conn_info local = {
        .qp_num      = qp->qp_num,
        .lid         = port_attr.lid,
        .rkey        = mr->rkey,
        .remote_addr = (uint64_t)(uintptr_t)file_buf,
        .file_size   = file_size,
    };
    memcpy(local.gid, &local_gid, 16);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT),
    };
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        perror("inet_pton");
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    struct conn_info remote;
    if (send_all(sock, &local, sizeof(local)) < 0) {
        perror("send local info");
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }
    if (recv_all(sock, &remote, sizeof(remote)) < 0) {
        perror("recv remote info");
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
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
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
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
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
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
        close(sock);
        ibv_destroy_qp(qp);
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(file_buf);
        fclose(fp);
        return 1;
    }

    if (!quiet) {
        printf("RDMA connection established! Sending file...\n");
    }

    uint64_t offset = 0;
    while (offset < file_size) {
        uint64_t this_chunk = file_size - offset;
        if (this_chunk > CHUNK_SIZE) {
            this_chunk = CHUNK_SIZE;
        }

        size_t got = fread(file_buf, 1, this_chunk, fp);
        if (got != this_chunk) {
            fprintf(stderr, "Short read at offset %llu: asked %llu, got %zu\n",
                    (unsigned long long)offset,
                    (unsigned long long)this_chunk,
                    got);
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }

        struct chunk_msg msg = {
            .offset = offset,
            .length = (uint32_t)this_chunk,
        };
        if (send_all(sock, &msg, sizeof(msg)) < 0) {
            perror("send chunk metadata");
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }

        struct ibv_sge sge = {
            .addr   = (uint64_t)(uintptr_t)file_buf,
            .length = (uint32_t)this_chunk,
            .lkey   = mr->lkey,
        };
        struct ibv_send_wr wr = {
            .wr_id      = offset,
            .opcode     = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED,
            .sg_list    = &sge,
            .num_sge    = 1,
            .wr.rdma = {
                /* The receiver reuses one chunk-sized registered buffer and
                 * applies the file offset in the TCP metadata during pwrite().
                 * Writing to remote_addr + offset would walk past the registered
                 * chunk buffer after the first transfer and corrupt later chunks.
                 */
                .remote_addr = remote.remote_addr,
                .rkey        = remote.rkey,
            },
        };
        struct ibv_send_wr *bad_wr = NULL;
        if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
            perror("ibv_post_send");
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }

        struct ibv_wc wc;
        while (ibv_poll_cq(cq, 1, &wc) == 0) {
            /* wait until the RDMA write is complete */
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Send failed at offset %llu: %s\n",
                    (unsigned long long)offset,
                    ibv_wc_status_str(wc.status));
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }

        int ready = 1;
        if (send_all(sock, &ready, sizeof(ready)) < 0) {
            perror("send data ready");
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }

        int ack = 0;
        if (recv_all(sock, &ack, sizeof(ack)) < 0) {
            perror("recv ack");
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }
        if (ack != 1) {
            fprintf(stderr, "Receiver rejected chunk at offset %llu\n",
                    (unsigned long long)offset);
            close(sock);
            ibv_destroy_qp(qp);
            ibv_destroy_cq(cq);
            ibv_dereg_mr(mr);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            free(file_buf);
            fclose(fp);
            return 1;
        }

        offset += this_chunk;
        if (!quiet && verbose) {
            double percent = 100.0 * (double)offset / (double)file_size;
            printf("Transferred %llu/%llu bytes (%.2f%%)\n",
                   (unsigned long long)offset,
                   (unsigned long long)file_size,
                   percent);
        }
    }

    if (!quiet) {
        if (summary || verbose) {
            printf("Transferred %llu/%llu bytes (100.00%%)\n",
                   (unsigned long long)file_size,
                   (unsigned long long)file_size);
        }
        printf("Transfer complete: %llu bytes sent successfully\n",
               (unsigned long long)file_size);
    }

    fclose(fp);
    close(sock);

    if (ibv_destroy_qp(qp) != 0)
        perror("ibv_destroy_qp");
    if (ibv_destroy_cq(cq) != 0)
        perror("ibv_destroy_cq");
    if (ibv_dereg_mr(mr) != 0)
        perror("ibv_dereg_mr");
    if (ibv_dealloc_pd(pd) != 0)
        perror("ibv_dealloc_pd");
    if (ibv_close_device(ctx) != 0)
        perror("ibv_close_device");

    free(file_buf);
    if (!quiet) {
        printf("File sent successfully via RDMA! ✅\n");
    }
    return 0;
}
