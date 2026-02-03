# Simple Open EtherCAT Master Library
[![Build Status](https://travis-ci.org/OpenEtherCATsociety/SOEM.svg?branch=master)](https://travis-ci.org/OpenEtherCATsociety/SOEM)
[![Build status](https://ci.appveyor.com/api/projects/status/bqgirjsxog9k1odf?svg=true)](https://ci.appveyor.com/project/hefloryd/soem-5kq8b)

This fork focuses on measuring and improving EtherCAT latency/jitter in SOEM.
It adds performance instrumentation and test harnesses for cycle timing analysis.

PERFORMANCE NOTES
=================

Clock mode (relative vs absolute)
---------------------------------

Some tests use `clock_nanosleep()` with `CLOCK_MONOTONIC`. You can choose
relative sleep or absolute (TIMER_ABSTIME) scheduling by toggling the
commented code paths in the test file.

IRQ affinity and PACKET_MMAP polling
------------------------------------

Disable irqbalance and pin the NIC IRQ to a dedicated CPU to reduce jitter,
especially when using PACKET_MMAP user-space busy polling (default in this
branch). Keep NIC IRQ handling off the busy-polling CPU.

Recommended procedure (example: reserve CPU2 for NIC IRQ/softirq and CPU3 for
the SOEM real-time thread):

1. Disable irqbalance.
```
sudo systemctl stop irqbalance
sudo systemctl disable irqbalance
```

2. Pin the NIC IRQ(s) to CPU2.
```
# Find IRQ(s) for eth0
grep -E "eth0|enp" /proc/interrupts

# Example: IRQ 131 -> CPU2 (bitmask 0x4)
sudo sh -c 'echo 4 > /proc/irq/131/smp_affinity'
```

3. Pin the SOEM real-time thread to CPU3 (example when launching the test).
```
sudo taskset -c 3 ./build/bin/cycle_test_2 eth0
```

4. Keep regular tasks off CPU2 and CPU3.
Runtime (systemd slices):
```
sudo systemctl set-property --runtime -- system.slice AllowedCPUs=0,1,4-7
sudo systemctl set-property --runtime -- user.slice AllowedCPUs=0,1,4-7
sudo systemctl set-property --runtime -- init.scope AllowedCPUs=0,1,4-7
```
Optional: move currently running tasks off CPU2 and CPU3.
```
ps -eLo pid,psr,comm | awk '$2==2 || $2==3 {print $1}' | xargs -r -n1 taskset -pc 0,1,4-7
```
Boot-time (kernel cmdline):
```
sudo sed -i.bak 's/isolcpus=domain,managed,3 nohz_full=3 rcu_nocbs=3/isolcpus=domain,managed_irq,2,3 nohz_full=2,3 rcu_nocbs=2,3/' /etc/default/grub
sudo update-grub
sudo reboot

grep isolcpus /etc/default/grub
```


To allow a small yield instead of full busy polling, set a non-zero sleep in
nanoseconds (e.g. 1000 for ~1us):

```
export EC_PACKET_MMAP_POLL_NS=1000
```

Trace marker collection
-----------------------

`test/linux/simple_test/cycle_test_2.c` writes outlier events to ftrace
`trace_marker` as `OUTLIER ...` lines. Use `collect_trace_marker.sh` to
collect those markers during a run.

BUILDING
========


Prerequisites for all platforms
-------------------------------

 * CMake 2.8.0 or later


Windows (Visual Studio)
-----------------------

 * Start a Visual Studio command prompt then:
   * `mkdir build`
   * `cd build`
   * `cmake .. -G "NMake Makefiles"`
   * `nmake`

Linux & macOS
--------------

   * `mkdir build`
   * `cd build`
   * `cmake ..`
   * `make`

ERIKA Enterprise RTOS
---------------------

 * Refer to http://www.erika-enterprise.com/wiki/index.php?title=EtherCAT_Master

Documentation
-------------

See https://openethercatsociety.github.io/doc/soem/


Want to contribute to SOEM or SOES?
-----------------------------------

If you want to contribute to SOEM or SOES you will need to sign a Contributor
License Agreement and send it to us either by e-mail or by physical mail. More
information is available in the [PDF](http://openethercatsociety.github.io/cla/cla_soem_soes.pdf).
