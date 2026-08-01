# RDMA Programming Guide
## Mellanox ConnectX-4 · SR-IOV VFs · RoCE · libibverbs

---

## Table of Contents

1. [What is RDMA?](#1-what-is-rdma)
2. [Architecture Overview](#2-architecture-overview)
3. [Key Concepts](#3-key-concepts)
4. [RDMA Operations](#4-rdma-operations)
5. [Installation](#5-installation)
6. [SR-IOV and Virtual Functions](#6-sr-iov-and-virtual-functions)
7. [Verification and Diagnostics](#7-verification-and-diagnostics)
8. [GID Index and RoCE vs InfiniBand](#8-gid-index-and-roce-vs-infiniband)
9. [Building the Test Programs](#9-building-the-test-programs)
10. [Running the Tests](#10-running-the-tests)
11. [Interpreting Results](#11-interpreting-results)
12. [Performance Tuning](#12-performance-tuning)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. What is RDMA?

**RDMA (Remote Direct Memory Access)** lets one machine read or write directly into another machine's memory over the network — bypassing the remote CPU, OS kernel, and TCP/IP stack entirely.

| Traditional TCP/IP | RDMA |
|--------------------|------|
| Data copied: NIC→kernel→userspace | Zero-copy: NIC↔application buffer |
| CPU involved on both sides | Remote CPU uninvolved (one-sided ops) |
| Latency: 50–200 µs | Latency: 1–5 µs (IB), 5–15 µs (RoCE VF) |
| Throughput limited by CPU | Throughput limited by link speed |

**Key use cases:** HPC (MPI), AI/ML training (NCCL), distributed storage (Ceph, NVMe-oF), in-memory databases, financial trading.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    User Application                      │
│                  (your rdma_pingpong)                    │
├─────────────────────────────────────────────────────────┤
│              libibverbs  (userspace verbs API)           │
│         libmlx5 / rdma-core  (provider driver)          │
├─────────────────────────────────────────────────────────┤
│         Kernel RDMA subsystem  (ib_core, rdma_cm)       │
│         mlx5_core / mlx5_ib  (kernel driver)            │
├─────────────────────────────────────────────────────────┤
│       Mellanox ConnectX-4  (PF → VFs via SR-IOV)        │
│            Physical loopback cable                       │
└─────────────────────────────────────────────────────────┘
```

**Data path** (send operation):
1. App writes to a registered memory buffer
2. App posts a Work Request (WR) to the Queue Pair (QP)
3. NIC DMAs data directly from app buffer → wire
4. Remote NIC DMAs data wire → remote app buffer
5. Completion posted to Completion Queue (CQ)

No kernel involvement after initial setup. No data copies.

---

## 3. Key Concepts

### Protection Domain (PD)
An isolation container. Memory regions and queue pairs belong to a PD. Resources in different PDs cannot interact.

```c
struct ibv_pd *pd = ibv_alloc_pd(ctx);
```

### Memory Region (MR)
A pinned, registered memory buffer the NIC can DMA to/from. Registration tells the NIC the physical pages behind a virtual address range and assigns access keys.

```c
struct ibv_mr *mr = ibv_reg_mr(pd, buf, size,
    IBV_ACCESS_LOCAL_WRITE |
    IBV_ACCESS_REMOTE_WRITE |   // needed for RDMA Write target
    IBV_ACCESS_REMOTE_READ);    // needed for RDMA Read source
```

Key fields after registration:
- `mr->lkey` — local key, used in SGEs for local access
- `mr->rkey` — remote key, shared with peer to allow remote access

### Queue Pair (QP)
A QP is a pair of queues: **Send Queue (SQ)** and **Receive Queue (RQ)**. It is the fundamental communication endpoint.

**QP Types:**

| Type | Description | Use case |
|------|-------------|----------|
| RC (Reliable Connected) | Point-to-point, guaranteed delivery, ordering | Most RDMA apps |
| UC (Unreliable Connected) | Point-to-point, no ACKs | Rare |
| UD (Unreliable Datagram) | Connectionless, multicast capable | All-to-all messaging |
| XRC | Extended RC for scalability | Large clusters |

All programs in this repo use **RC**.

**QP State Machine:**

```
RESET → INIT → RTR (Ready to Receive) → RTS (Ready to Send)
```

Each transition requires `ibv_modify_qp()` with specific attributes.

### Completion Queue (CQ)
A ring buffer where completed Work Requests are deposited. Both send and recv completions land here (or you can use separate CQs for each).

```c
struct ibv_cq *cq = ibv_create_cq(ctx, depth, NULL, NULL, 0);

// Poll for completions (spin loop — lowest latency)
struct ibv_wc wc;
while (ibv_poll_cq(cq, 1, &wc) == 0) {}

// Or use a completion channel (event-driven — lower CPU, higher latency)
struct ibv_comp_channel *ch = ibv_create_comp_channel(ctx);
```

### Work Request (WR) and Scatter-Gather Entry (SGE)
A WR describes an operation to perform. It references one or more SGEs that point to registered memory buffers.

```c
struct ibv_sge sge = {
    .addr   = (uint64_t)buf,   // virtual address
    .length = size,
    .lkey   = mr->lkey,
};

struct ibv_send_wr wr = {
    .opcode     = IBV_WR_SEND,
    .send_flags = IBV_SEND_SIGNALED,  // generate completion
    .sg_list    = &sge,
    .num_sge    = 1,
};
```

### Address Handle (AH)
For UD QPs, an AH describes the path to a remote endpoint (GID, LID, SL). Not needed for RC QPs (path is set during QP connect).

---

## 4. RDMA Operations

### Two-Sided Operations (both CPUs involved)

#### Send / Receive
The sender posts a SEND; the receiver must have a matching RECV posted. Both sides get completions.

```
Client                          Server
──────                          ──────
post_recv()  ←── pre-posted
post_send()  ──────────────→   poll_cq() → data in recv buffer
poll_cq()                      post_send() ──────────────→
             ←──────────────   poll_cq()
poll_cq() → data in recv buf
```

**Use when:** you need the remote side to control when it receives data, or for request/response protocols.

### One-Sided Operations (initiator only, remote CPU idle)

#### RDMA Write
Initiator **pushes** data directly into remote memory. Remote CPU gets no notification.

```
Client                          Server
──────                          ──────
post_write(raddr, rkey) ──→    [data appears in server buf]
poll_cq()                      [server CPU does nothing]
```

```c
struct ibv_send_wr wr = {
    .opcode  = IBV_WR_RDMA_WRITE,
    .wr.rdma = { .remote_addr = raddr, .rkey = rkey },
};
```

**Requires:** server MR registered with `IBV_ACCESS_REMOTE_WRITE`.  
**Use when:** streaming data to a remote buffer, producer/consumer rings, GPU-direct.

#### RDMA Write with Immediate
Like RDMA Write but delivers a 32-bit immediate value to the remote CQ, triggering a recv completion. Lets the remote side know data arrived.

```c
wr.opcode         = IBV_WR_RDMA_WRITE_WITH_IMM;
wr.imm_data       = htonl(my_tag);
```

**Use when:** you want RDMA Write performance but need the remote side to get notified.

#### RDMA Read
Initiator **pulls** data out of remote memory. Remote CPU does nothing.

```
Client                          Server
──────                          ──────
post_read(raddr, rkey) ───→    [NIC reads from server buf]
poll_cq() → data in local buf  [server CPU does nothing]
```

```c
struct ibv_send_wr wr = {
    .opcode  = IBV_WR_RDMA_READ,
    .wr.rdma = { .remote_addr = raddr, .rkey = rkey },
};
```

**Requires:** server MR registered with `IBV_ACCESS_REMOTE_READ`.  
**Use when:** fetching specific records from a remote data structure (e.g., hash table lookup).

### Atomic Operations
Both sides must use RC QPs with `max_rd_atomic` / `max_dest_rd_atomic` set.

| Operation | Description |
|-----------|-------------|
| `IBV_WR_ATOMIC_FETCH_AND_ADD` | Atomically add to a 64-bit value, return old |
| `IBV_WR_ATOMIC_CMP_AND_SWP`  | Compare-and-swap a 64-bit value |

**Use when:** distributed locks, counters, sequence numbers without a server process.

### Operation Comparison

| Property | Send/Recv | RDMA Write | RDMA Write+Imm | RDMA Read | Atomic |
|----------|-----------|------------|----------------|-----------|--------|
| Remote CPU needed | Yes | No | Recv notified | No | No |
| Remote recv WR needed | Yes | No | Yes | No | No |
| Remote completion | Yes | No | Yes | No | No |
| Latency | Medium | Low | Low | Low | Low |
| Good for streaming | Moderate | Excellent | Good | Poor | No |
| Good for fetch | No | No | No | Yes | Yes |

---

## 5. Installation

### Ubuntu / Debian

```bash
# Core RDMA userspace libraries
sudo apt install -y \
    libibverbs-dev \
    libibverbs1 \
    ibverbs-utils \
    rdma-core \
    librdmacm-dev \
    libibumad-dev

# Diagnostic and performance tools
sudo apt install -y \
    infiniband-diags \
    perftest \
    ibutils \
    iproute2          # includes 'rdma' command
```

### RHEL / CentOS / Rocky

```bash
sudo dnf install -y \
    libibverbs \
    libibverbs-devel \
    libibverbs-utils \
    rdma-core \
    rdma-core-devel \
    infiniband-diags \
    perftest \
    iproute-rdma
```

### MLNX_OFED (Mellanox Recommended)

For ConnectX-4, Mellanox's own OFED stack often provides better performance and more features than the inbox drivers.

```bash
# Download from: https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/
# Choose your OS version and architecture

tar xf MLNX_OFED_LINUX-*.tgz
cd MLNX_OFED_LINUX-*/
sudo ./mlnxofedinstall --add-kernel-support

# Restart RDMA services
sudo /etc/init.d/openibd restart
# or
sudo systemctl restart openibd
```

Verify driver version:
```bash
ofed_info -s
# MLNX_OFED_LINUX-5.x-x.x.x.x:
```

---

## 6. SR-IOV and Virtual Functions

### What is SR-IOV?

**SR-IOV (Single Root I/O Virtualization)** allows a single physical PCIe device (the Physical Function, PF) to present multiple virtual devices (Virtual Functions, VFs) to the OS and hypervisor. Each VF can be passed directly into a VM, giving that VM near-native hardware access to the NIC.

```
Physical Host
├── ConnectX-4 PF (mlx5_0)
│   ├── VF 0  → passed to VM1 (appears as rocep7s0)
│   └── VF 1  → passed to VM2 (appears as rocep7s0)
└── loopback cable: port1 ↔ port2
```

### Enabling VFs on the Host (PF side)

```bash
# Check current VF count
cat /sys/class/net/ens3f0/device/sriov_numvfs

# Enable 2 VFs
echo 2 | sudo tee /sys/class/net/ens3f0/device/sriov_numvfs

# Set VF to trust mode (needed for some RoCE configurations)
sudo ip link set ens3f0 vf 0 trust on
sudo ip link set ens3f0 vf 1 trust on

# Persist across reboots (add to /etc/rc.local or udev rules)
```

### Pass VF to VM (libvirt / QEMU)

```xml
<!-- In VM XML definition -->
<hostdev mode='subsystem' type='pci' managed='yes'>
  <source>
    <address domain='0x0000' bus='0x07' slot='0x10' function='0x0'/>
  </source>
</hostdev>
```

Or via QEMU command line:
```bash
qemu-system-x86_64 \
  -device vfio-pci,host=07:10.0 \
  ...
```

### Verify VF in the VM

```bash
# List RDMA devices
ibv_devices
# Expected: rocep7s0 or similar

# Check device info
ibv_devinfo -d rocep7s0

# Verify port state
rdma link show
# Expected: link rocep7s0/1 state ACTIVE ...
```

### Network Configuration for RoCE

Since your VFs operate in RoCE (Ethernet) mode, the VMs need IP addresses on the RDMA interface:

```bash
# VM1
sudo ip addr add 192.168.100.1/24 dev enp7s0
sudo ip link set enp7s0 up

# VM2
sudo ip addr add 192.168.100.2/24 dev enp7s0
sudo ip link set enp7s0 up

# Test basic connectivity first
ping 192.168.100.2
```

---

## 7. Verification and Diagnostics

### List RDMA Devices

```bash
ibv_devices
```
```
device          	   node GUID
------          	   ----------------
rocep7s0        	   xxxxxxxxxxxx
```

### Detailed Device Info

```bash
ibv_devinfo -d rocep7s0 -v
```

Key fields to check:
- `state: PORT_ACTIVE` — port is up and connected
- `link_layer: Ethernet` — RoCE mode (vs `InfiniBand`)
- `GID[0]: ...` — available GIDs for RoCE routing
- `max_mr_size` — maximum single memory region
- `max_qp_wr` — maximum WRs per QP

### Port State

```bash
ibstat rocep7s0
# or
rdma link show
```

Expected output:
```
link rocep7s0/1 state ACTIVE physical_state POLLING ...
```

### List Available GIDs

```bash
ibv_devinfo -d rocep7s0 -v | grep -A1 GID
```
```
GID[  0]:               fe80:0000:0000:0000:xxxx:xxxx:xxxx:xxxx
GID[  1]:               0000:0000:0000:0000:0000:0000:0000:0000
GID[  2]:               0000:0000:0000:0000:0000:0000:0000:0000
GID[  3]:               0000:0000:0000:0000:0000:ffff:c0a8:6401
```

GID index 3 here is an IPv4-mapped GID (`::ffff:192.168.100.1`) — this is what you used successfully.

### Check Firmware and Driver

```bash
# Firmware version
ibv_devinfo -d rocep7s0 | grep fw_ver

# Driver version
modinfo mlx5_ib | grep ^version

# RDMA kernel modules loaded
lsmod | grep -E 'mlx5|rdma|ib_'
```

### Traffic Counters

```bash
# Port counters (bytes, packets, errors)
cat /sys/class/infiniband/rocep7s0/ports/1/counters/*

# Or using perfquery (if using IB mode)
perfquery -d rocep7s0 -p 1
```

### Standard perftest Benchmarks

The `perftest` package provides reference implementations. Compare your results against these:

```bash
# Send latency (equivalent to rdma_pingpong)
# VM1:
ib_send_lat -d rocep7s0 -i 1 -x 3
# VM2:
ib_send_lat -d rocep7s0 -i 1 -x 3 192.168.100.1

# Write bandwidth (equivalent to rdma_write_bw)
# VM1:
ib_write_bw -d rocep7s0 -i 1 -x 3
# VM2:
ib_write_bw -d rocep7s0 -i 1 -x 3 192.168.100.1

# Read latency (equivalent to rdma_read_lat)
# VM1:
ib_read_lat -d rocep7s0 -i 1 -x 3
# VM2:
ib_read_lat -d rocep7s0 -i 1 -x 3 192.168.100.1
```

The `-x 3` flag specifies GID index 3 (matching your setup).

---

## 8. GID Index and RoCE vs InfiniBand

### InfiniBand Mode (LID-based)
- Ports have a 16-bit **LID (Local Identifier)** assigned by the subnet manager
- QPs addressed by `(LID, QPN)` pair
- LID is always non-zero when a subnet manager is running
- Your setup: **LID = 0x0000** → not InfiniBand mode

### RoCE Mode (GID-based)
- **RoCE v1**: RDMA over Ethernet, same L2 broadcast domain required
- **RoCE v2**: RDMA over UDP/IP, routable across subnets (more common)
- Ports have **GIDs (Global Identifiers)** — 128-bit addresses similar to IPv6
- GID types:

| GID Type | Format | Example |
|----------|--------|---------|
| Link-local | fe80::/10 | `fe80::...` |
| IPv6-mapped | ::ffff:x.x.x.x | `::ffff:192.168.100.1` |
| Pure IPv6 | Global unicast | `2001:db8::1` |

### Finding Your GID Index

```bash
ibv_devinfo -d rocep7s0 -v | grep GID
```

Pick an index with a non-zero GID that matches your RDMA interface IP:
- `::ffff:192.168.100.1` matches IP `192.168.100.1` → IPv4-mapped RoCE v2

Your confirmed working index: **GID index 3** (`::ffff:c0a8:6401` = `192.168.100.1`)

### When to Use Each Flag

```bash
# Pure InfiniBand (LID != 0, subnet manager running)
./rdma_pingpong -d mlx5_0 -i 1

# RoCE with explicit GID index
./rdma_pingpong -d rocep7s0 -i 1 -g 3

# Auto-detect (programs detect LID=0 and default to GID index 0)
./rdma_pingpong -d rocep7s0 -i 1
```

---

## 9. Building the Test Programs

### Prerequisites

```bash
sudo apt install -y libibverbs-dev gcc make
```

### Build

```bash
git clone <repo> && cd rdma/
make
```

Or individually:
```bash
gcc -O2 -Wall -o rdma_pingpong  rdma_pingpong.c  -libverbs
gcc -O2 -Wall -o rdma_write_bw  rdma_write_bw.c  -libverbs
gcc -O2 -Wall -o rdma_read_lat  rdma_read_lat.c  -libverbs
```

### Deploy to Both VMs

```bash
# From your Mac (or any build host)
for vm in vm1 vm2; do
  ssh $vm "mkdir -p ~/rdma"
  scp rdma_pingpong rdma_write_bw rdma_read_lat $vm:~/rdma/
done
```

---

## 10. Running the Tests

### Common Options (all programs)

| Flag | Description | Default |
|------|-------------|---------|
| `-d <dev>` | RDMA device name | first found |
| `-i <n>` | IB port number | 1 |
| `-p <n>` | TCP port for setup | varies |
| `-n <n>` | Iterations | varies |
| `-s <n>` | Message/transfer size (bytes) | varies |
| `-g <n>` | GID index (-1 = auto) | -1 |

### Test 1: Send/Receive Latency (`rdma_pingpong`)

Measures round-trip latency of two-sided Send/Receive operations.  
Both CPUs are involved: server must post a Recv WR and then reply with a Send.

```bash
# VM1 (server)
./rdma_pingpong -d rocep7s0 -i 1 -g 3 -n 10000

# VM2 (client)
./rdma_pingpong -d rocep7s0 -i 1 -g 3 -n 10000 192.168.100.1
```

**Expected output (client):**
```
=== Results (10000 iters, 64 bytes/msg) ===
  RTT latency (us):
    min      :     9.10
    avg      :    10.45
    median   :    10.20
    p99      :    13.50
    p99.9    :    18.00
    max      :   180.00
  One-way (avg): 5.22 us
  Throughput:    95694 msg/s
```

### Test 2: RDMA Write Bandwidth (`rdma_write_bw`)

Measures throughput of one-sided RDMA Write operations.  
Server CPU is **idle** during the test — only the NICs are active.

```bash
# VM1 (server — passive)
./rdma_write_bw -d rocep7s0 -i 1 -g 3 -s 65536 -n 5000

# VM2 (client — writes to server memory)
./rdma_write_bw -d rocep7s0 -i 1 -g 3 -s 65536 -n 5000 192.168.100.1
```

**Expected output (client):**
```
=== RDMA Write Bandwidth Results (5000 iters, 65536 bytes) ===
  Elapsed:    0.521 s
  Bandwidth:  5.04 Gbps
  Msg rate:   0.010 Mpps
```

**Size sweep for BW curve:**
```bash
for size in 256 1024 4096 16384 65536 262144 1048576; do
  echo -n "size=$size  "
  ./rdma_write_bw -d rocep7s0 -i 1 -g 3 -s $size -n 1000 192.168.100.1 2>/dev/null | grep Bandwidth
done
```

### Test 3: RDMA Read Latency (`rdma_read_lat`)

Measures latency of one-sided RDMA Read operations.  
Server CPU is **completely idle** — it doesn't even know reads are happening.

```bash
# VM1 (server — its memory is read remotely)
./rdma_read_lat -d rocep7s0 -i 1 -g 3

# VM2 (client — reads from server memory)
./rdma_read_lat -d rocep7s0 -i 1 -g 3 192.168.100.1
```

**Expected output (client):**
```
=== RDMA Read Latency Results (1000 iters, 64 bytes) ===
  NOTE: server CPU was idle during this entire measurement.

  Latency (us):
    min      :    10.20
    avg      :    11.35
    median   :    11.00
    p99      :    14.20
    p99.9    :   110.00
    max      :   110.00
  Throughput : 88106 reads/s
```

RDMA Read latency is typically slightly higher than RDMA Write because the initiator has to wait for the data to come back across the wire before completion.

---

## 11. Interpreting Results

### Latency Numbers

| Metric | What it tells you |
|--------|-----------------|
| `min` | Best-case hardware latency (cache-warm, no jitter) |
| `median (p50)` | Typical latency under normal load |
| `p99` | Tail latency — what 1% of requests experience |
| `p99.9` | Extreme tail — needs ≥10,000 iterations to be meaningful |
| `max` | Worst single event (VM scheduling pause, RNR retry, etc.) |
| `one-way (avg/2)` | Estimated single-direction latency |

### Understanding the avg vs median gap

If `avg` >> `median`, a small number of outliers are pulling the average up:

```
Your result:  avg=18.63 µs  median=10.40 µs  max=2104 µs
```

This means ~998 iterations landed near 10 µs and 1–2 had a 2 ms pause (VM scheduling jitter). The median is the real hardware latency.

**Fix:** run more iterations (`-n 10000`) and pin CPUs:
```bash
taskset -c 2 ./rdma_pingpong ...
```

### Bandwidth Numbers

```
Bandwidth = (iters × size × 8 bits) / elapsed_seconds  [Gbps]
Msg rate  = iters / elapsed_seconds                     [Mpps]
```

ConnectX-4 theoretical max: **40 Gbps** (single port). VF overhead and loopback reduce this. Expect 5–15 Gbps for 64KB messages through a VF pair.

### Comparing Send vs Write vs Read Latency

Typical order (lowest to highest latency):
```
RDMA Write ≤ RDMA Read < Send/Recv
```

RDMA Write is fastest because it's truly fire-and-forget — no round trip needed.  
RDMA Read adds a full round trip (data must travel back to initiator).  
Send/Recv adds server CPU processing time on top of the round trip.

---

## 12. Performance Tuning

### CPU Affinity
Pin application threads to CPUs local to the NIC's NUMA node:
```bash
# Find NIC's NUMA node
cat /sys/class/net/enp7s0/device/numa_node

# Run pinned to that NUMA node
numactl --cpunodebind=0 --membind=0 ./rdma_pingpong ...
# or
taskset -c 4 ./rdma_pingpong ...
```

### Interrupt Affinity
Move NIC IRQs away from the application CPU:
```bash
# Find IRQs for your interface
grep enp7s0 /proc/interrupts | awk '{print $1}' | tr -d ':'

# Set affinity to CPU 0 (move away from app CPU)
echo 1 > /proc/irq/<N>/smp_affinity
```

### Huge Pages (reduces TLB misses for large MRs)
```bash
echo 512 | sudo tee /proc/sys/vm/nr_hugepages
# In your app, use mmap with MAP_HUGETLB instead of posix_memalign
```

### MLX5 Tuning

```bash
# Check current adaptive moderation settings
ethtool -c enp7s0

# Disable interrupt coalescing for minimum latency
ethtool -C enp7s0 rx-usecs 0 tx-usecs 0

# Enable busy polling (reduces latency at cost of CPU)
echo 50 > /proc/sys/net/core/busy_read
echo 50 > /proc/sys/net/core/busy_poll
```

### QoS and ECN for RoCE (prevents packet drops)
```bash
# Check PFC (Priority Flow Control) status
mlnx_qos -i enp7s0

# Enable ECN (Explicit Congestion Notification)
cma_roce_tos -d rocep7s0 -t 106
```

### Key QP Parameters for Low Latency

| Parameter | Value | Effect |
|-----------|-------|--------|
| `timeout` | 8–14 | ACK timeout (lower = faster recovery) |
| `retry_cnt` | 7 | Retransmit count |
| `rnr_retry` | 7 | Receiver-not-ready retry (7 = infinite) |
| `min_rnr_timer` | 1 | Minimal RNR backoff time |
| `max_inline_data` | 64–256 | Inline data avoids DMA for small messages |

**Inline data** is especially impactful for small messages:
```c
attr.cap.max_inline_data = 256;  // in ibv_qp_init_attr

// Then in send WR:
wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
// Data is copied into the NIC's send queue directly — no DMA, lower latency
```

---

## 13. Troubleshooting

### `ibv_modify_qp → RTR: Invalid argument`
**Cause:** Port is RoCE (LID=0) but code is trying to use LID-based routing.  
**Fix:** Use `-g <idx>` with a valid GID index, or let auto-detection pick one.
```bash
ibv_devinfo -d rocep7s0 -v | grep GID  # find non-zero GIDs
./rdma_pingpong -d rocep7s0 -g 3 ...
```

### `No RDMA devices found`
```bash
lsmod | grep mlx5_ib          # driver loaded?
ls /sys/class/infiniband/      # devices visible to kernel?
dmesg | grep -i "mlx5\|vfio"  # VF passthrough errors?
```

### Port is `PORT_DOWN` or `PORT_INIT`
```bash
rdma link show                   # check link state
ip link show enp7s0              # is the Ethernet interface up?
sudo ip link set enp7s0 up       # bring it up
ethtool enp7s0 | grep Link       # physical link detected?
```

### `WC error: retry exceeded`
**Cause:** Packets are being dropped — wrong GID, wrong subnet, firewall, link issue.
```bash
# Verify IP connectivity first
ping 192.168.100.2

# Check for packet loss
ethtool -S enp7s0 | grep -i "drop\|error\|miss"

# Verify both sides use same GID type (IPv4-mapped vs link-local)
ibv_devinfo -d rocep7s0 -v | grep GID
```

### `WC error: local protection error`
**Cause:** Posted a WR with a stale lkey (MR was deregistered) or wrong lkey.  
**Fix:** Make sure `mr->lkey` is used in SGEs, not a cached copy from before re-registration.

### High Latency Spikes (p99.9 >> p50)
1. **VM scheduling jitter** — pin to a dedicated CPU with `taskset`
2. **Memory allocation during test** — pre-allocate all buffers before timing
3. **RNR retries** — increase `rnr_retry` to 7 and `min_rnr_timer` to 1
4. **Frequency scaling** — disable C-states: `cpupower frequency-set -g performance`

### `perftest` works but custom code doesn't
Compare `ibv_devinfo` output for both. Common mismatches:
- Wrong GID index
- QP max_rd_atomic/max_dest_rd_atomic mismatch between peers
- MR access flags missing `REMOTE_WRITE` or `REMOTE_READ`
- MTU mismatch (use `IBV_MTU_1024` for VFs if 4096 fails)

---

## Quick Reference Card

```bash
# Environment check
ibv_devices                                    # list devices
ibv_devinfo -d rocep7s0 -v | grep -E 'state|link_layer|GID'
rdma link show

# Run tests (RoCE, GID index 3)
# VM1:
./rdma_pingpong  -d rocep7s0 -g 3 -n 10000
./rdma_write_bw  -d rocep7s0 -g 3 -n 5000 -s 65536
./rdma_read_lat  -d rocep7s0 -g 3 -n 5000

# VM2:
./rdma_pingpong  -d rocep7s0 -g 3 -n 10000  192.168.100.1
./rdma_write_bw  -d rocep7s0 -g 3 -n 5000 -s 65536  192.168.100.1
./rdma_read_lat  -d rocep7s0 -g 3 -n 5000  192.168.100.1

# Reference benchmarks (perftest)
ib_send_lat  -d rocep7s0 -x 3 [server-ip]
ib_write_bw  -d rocep7s0 -x 3 [server-ip]
ib_read_lat  -d rocep7s0 -x 3 [server-ip]
```
