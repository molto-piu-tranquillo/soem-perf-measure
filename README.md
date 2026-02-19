# SOEM — EtherCAT Latency Optimization Fork (AF_XDP)

Fork of [SOEM](https://github.com/OpenEtherCATsociety/SOEM) focused on
send-recv latency/jitter reduction for real-time EtherCAT on Linux RT.

This branch (`feat/af-xdp`) uses AF_XDP busy-poll for IRQ-free NAPI — the
kernel's NAPI poll runs inline in the SOEM thread context, bypassing
ksoftirqd and hardware interrupts entirely.

> **Note:** `feat/uio` (Pseudo-DPDK) 브랜치가 AF_XDP보다 더 우수한 성능을 보입니다.
> AF_XDP는 이전 최적화 버전입니다.

## Branches

| Branch | Transport | Description |
|--------|-----------|-------------|
| `main` | AF_PACKET | Stock SOEM + perf instrumentation |
| `feat/af-xdp` | AF_XDP | Zero-copy busy-poll, IRQ-free NAPI (이 브랜치) |
| `feat/uio` | **UIO (DPDK-style)** | Kernel bypass, zero-syscall TX/RX (최적 버전) |

## Prerequisites

- Linux 6.8+ with PREEMPT_RT
- `libbpf` development library (`libbpf-dev`)
- r8169_xdp 커널 드라이버 (XDP/AF_XDP 지원)
- CMake 2.8+

## Build

```bash
mkdir build && cd build
cmake ..    # USE_AF_XDP defaults to ON on this branch
make
```

> `USE_AF_XDP`는 이 브랜치에서 기본 `ON`입니다.
> AF_PACKET으로 빌드하려면 `cmake -DUSE_AF_XDP=OFF ..`

## Usage

```bash
# 1. Load r8169_xdp driver (AF_XDP/XSK 지원 필수)
cd /path/to/r8169_xdp && make && make install

# 2. Apply runtime optimizations (reboot 시 리셋, 매번 재실행 필요)
sudo systemctl stop irqbalance && sudo systemctl disable irqbalance
sudo chrt -f -p 99 $(pgrep -x "ksoftirqd/3")
sudo mount -t resctrl resctrl /sys/fs/resctrl 2>/dev/null || true
sudo mkdir -p /sys/fs/resctrl/rt_cpu3
echo 3      | sudo tee /sys/fs/resctrl/rt_cpu3/cpus_list > /dev/null
echo "L2:0=03ff" | sudo tee /sys/fs/resctrl/schemata > /dev/null
echo "L2:0=fc00" | sudo tee /sys/fs/resctrl/rt_cpu3/schemata > /dev/null

# 3. Run
sudo ./build/test/linux/simple_test/cycle_test_2 eth0

# 4. Verify IRQ-free operation (in another terminal)
watch -n 1 "grep eth0 /proc/interrupts"   # count should NOT increase

# 5. Restore original driver when done
cd /path/to/r8169_xdp && make uninstall
```

> `cycle_test_2`는 내부적으로 CPU3 pinning과 SCHED_FIFO 99를 설정하므로
> 외부 `chrt`/`taskset`은 불필요합니다.

## AF_XDP Busy-Poll Architecture

```
┌────────── CPU3 (isolated, SOEM FIFO 99) ──────────┐
│                                                     │
│  SOEM cycle loop:                                   │
│    ecx_outframe: txbuf → memcpy → UMEM → TX ring   │
│    ecx_recvpkt:                                     │
│      peek XSK RX ring → miss?                      │
│        → sendto() [sets NAPI_STATE_SCHED]           │
│        → poll(0) [napi_busy_loop inline]            │
│            ├─ rtl8169_xsk_tx(): TX processing       │
│            ├─ rtl_rx_zc(): RX processing            │
│            └─ napi_complete_done: clear SCHED       │
│      peek XSK RX ring → hit → memcpy → tempbuf     │
│                                                     │
│  HW IRQ: disabled (IntrMask=0)                      │
│  ksoftirqd: not involved                            │
└─────────────────────────────────────────────────────┘
```

## System Setup

아래 설정이 모두 적용되어야 최적 성능(P99 ≤ 28μs)을 달성합니다.

**BIOS**:
- Intel SpeedStep: `Disabled`
- C-state: `Disabled`

**Boot parameters** (커널 부트 옵션, `/etc/default/grub`에 설정):
```
isolcpus=domain,managed,3 nohz_full=3 rcu_nocbs=3
processor.max_cstate=1 processor_idle.max_cstate=1 intel_idle.max_cstate=0
clocksource=tsc tsc=reliable idle=poll noht
intel_pstate=disable nmi_watchdog=0 nosoftlockup
irqaffinity=0-2 iommu=pt hpet=disable
rcupdate.rcu_cpu_stall_suppress=1
net.ifnames=0 biosdevname=0
```

**Runtime** (reboot 시 리셋, 매번 재실행 필요):
- irqbalance 비활성화: `sudo systemctl stop irqbalance && sudo systemctl disable irqbalance`
- IRQ affinity (IRQ 131 → CPU3): `echo 8 | sudo tee /proc/irq/131/smp_affinity > /dev/null`
  - 주의: DPDK bind/unbind 시 IRQ 번호가 변경될 수 있으므로 `/proc/interrupts`에서 확인 필요
- ksoftirqd FIFO 99: `sudo chrt -f -p 99 $(pgrep -x "ksoftirqd/3")`
- CAT L2 6-way: `resctrl`로 CPU3에 L2 768KB 전용 할당 (위 스크립트 참조)
