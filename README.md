# SOEM — EtherCAT Latency Optimization Fork

Fork of [SOEM](https://github.com/OpenEtherCATsociety/SOEM) focused on
send-recv latency/jitter reduction for real-time EtherCAT on Linux RT.

## Branches

| Branch | Transport | Description |
|--------|-----------|-------------|
| `main` | AF_PACKET | Stock SOEM + perf instrumentation |
| `feat/af-xdp` | AF_XDP | Zero-copy busy-poll, IRQ-free NAPI |
| `feat/uio` | **UIO (DPDK-style)** | Kernel bypass, zero-syscall TX/RX (최적 버전, 이 브랜치) |

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
# or
sudo ./build/test/linux/simple_test/cycle_test_2 0000:01:00.0

# 4. Restore kernel driver
sudo /path/to/r8169_uio/scripts/bind_uio.sh unbind
```

> UIO bind 후 `eth0`이 사라지지만 PCI BDF(`0000:01:00.0`)로 정상 동작합니다.
> `eth0`을 인자로 줘도 내부적으로 기본 BDF로 fallback합니다.

### feat/af-xdp (AF_XDP transport)

r8169_xdp 커널 드라이버가 로드된 상태에서 실행:

```bash
# 1. Load r8169_xdp driver
cd /path/to/r8169_xdp && make && make install

# 2. Apply runtime optimizations (동일)
# ... (위 feat/uio와 동일한 스크립트)

# 3. Run
sudo ./build/test/linux/simple_test/cycle_test_2 eth0

# 4. Restore original driver
cd /path/to/r8169_xdp && make uninstall
```

### main (AF_PACKET transport)

```bash
sudo ./build/test/linux/simple_test/cycle_test_2 eth0
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

> `cycle_test_2`는 내부적으로 CPU3 pinning과 SCHED_FIFO 99를 설정하므로
> 외부 `chrt`/`taskset`은 불필요합니다.
