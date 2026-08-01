CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -g
LDFLAGS = -libverbs

TARGETS = rdma_pingpong rdma_write_bw rdma_read_lat

UNAME := $(shell uname)

.PHONY: all clean

all:
ifeq ($(UNAME), Darwin)
	$(error libibverbs is Linux-only. Copy sources to your Linux VMs:\
	  scp *.c Makefile user@vm1:~/rdma/ && ssh user@vm1 "cd ~/rdma && make")
endif
	$(MAKE) $(TARGETS)

rdma_pingpong: rdma_pingpong.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rdma_write_bw: rdma_write_bw.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rdma_read_lat: rdma_read_lat.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)
