# SOEM — EtherCAT Latency Optimization Fork

Fork of [SOEM](https://github.com/OpenEtherCATsociety/SOEM) focused on
send-recv latency/jitter reduction for real-time EtherCAT on Linux RT.

## Branches

| Branch | Transport | Description |
|--------|-----------|-------------|
| `main` | AF_PACKET | Stock SOEM + perf instrumentation |
| `feat/af-xdp` | AF_XDP | Zero-copy busy-poll, IRQ-free NAPI |
| `feat/uio` | **UIO (DPDK-style)** | Kernel bypass, zero-syscall TX/RX |

## Build

```bash
mkdir build && cd build
cmake ..
make
```

feat/uio 브랜치는 r8169_uio 드라이버 소스를 `oshw/linux/`에 직접 포함하고 있어
외부 의존성 없이 빌드 가능.

## Usage

### feat/uio (UIO transport)

```bash
# 1. Bind NIC to UIO
sudo /path/to/r8169_uio/scripts/bind_uio.sh bind

# 2. Run
sudo ./build/test/linux/simple_test/cycle_test_2 eth0
# or
sudo ./build/test/linux/simple_test/cycle_test_2 0000:01:00.0

# 3. Restore kernel driver
sudo /path/to/r8169_uio/scripts/bind_uio.sh unbind
```

### main / feat/af-xdp

```bash
sudo ./build/test/linux/simple_test/cycle_test_2 eth0
```

## System Setup

- CPU isolation: `isolcpus=3 nohz_full=3 rcu_nocbs=3`
- RT scheduling: `chrt -f 99 taskset -c 3 ./cycle_test_2 eth0`
- CAT L2 (optional): `resctrl` 로 CPU3에 L2 way 할당
- IRQ affinity: `irqbalance` 비활성화
