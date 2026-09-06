# Linux Tuning for Co-located Deployment

`README.md` says CPU pinning and `SCHED_FIFO` priority belong in the
application's startup sequence, not in this library, and points here for
the full setup. This is that setup: what to configure at the OS/kernel
level, and what the application itself needs to do, for the busy-poll
receive loop in `receiver.hpp` to actually get the latency it's designed
for. None of this is exercised by CI or `BENCHMARK_RESULTS.md` — it
requires a real host (isolated cores, a real NIC), not a container — so
it's documented as a runbook, not validated with numbers the way the rest
of this repo's claims are.

---

## 1. Why any of this is necessary

`MulticastReceiver::run()` busy-polls `recvmsg(MSG_DONTWAIT)` in a tight
loop (see `receiver.hpp` and the "Why not epoll?" note in `README.md`).
That design assumption only holds if the thread running it:

- never gets preempted by the scheduler running something else on the
  same core,
- never gets migrated to a different core mid-run (which would cost a
  cache-cold restart and, on some hardware, a TSC resync),
- never shares its core with interrupt handling for unrelated devices.

Every step below exists to make one of those three things true. Skipping
any of them doesn't break correctness — `GapBuffer` and the order book
are still correct — it just reintroduces the scheduler jitter that
`SO_TIMESTAMPING`'s software timestamp already measures honestly (see
`BENCHMARK_RESULTS.md`'s `kernel_rx_sw_ts -> read` figure: that gap is
exactly what an un-tuned host adds on top of the numbers in this
document).

---

## 2. Kernel boot parameters

Add to the kernel command line (`/etc/default/grub`'s
`GRUB_CMDLINE_LINUX`, then `update-grub` and reboot):

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

- `isolcpus=2,3` — removes cores 2 and 3 from the general scheduler's
  load-balancing pool. The receiver thread (and, if pinned separately,
  the decision-engine thread) should run only on these cores.
- `nohz_full=2,3` — stops the periodic scheduler tick on those cores
  once only one runnable task is present, removing a periodic
  interrupt source.
- `rcu_nocbs=2,3` — offloads RCU callback processing off the isolated
  cores onto a housekeeping core, so kernel RCU work doesn't compete for
  cycles on the hot cores.

Pick core numbers based on `lscpu`'s NUMA topology — the isolated cores
should be on the same NUMA node as the NIC (`cat
/sys/class/net/<iface>/device/numa_node`).

---

## 3. IRQ affinity

By default, the NIC's receive-queue interrupts can land on any core,
including the isolated ones, which would inject exactly the kind of
jitter isolation is meant to prevent.

```bash
# Find the NIC's IRQ numbers
grep <iface> /proc/interrupts

# Pin each one away from the isolated cores (example: force onto core 0)
echo 1 > /proc/irq/<irq_number>/smp_affinity
```

Most distros run `irqbalance` by default, which will fight this — either
stop it (`systemctl stop irqbalance`) or add the isolated cores to its
ban list (`IRQBALANCE_BANNED_CPUS` in `/etc/default/irqbalance` or
`/etc/sysconfig/irqbalance`, distro-dependent).

If the NIC driver supports it, set the receive-queue count to match the
number of housekeeping cores, not the isolated ones, so the NIC never
even offers an interrupt to an isolated core.

---

## 4. Application-side pinning and scheduling

In the receiver process's startup sequence — `main.cpp` in this repo
does not do this, deliberately (see README) — before calling
`receiver.run()`:

```cpp
#include <pthread.h>
#include <sched.h>

void pin_and_prioritize(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    sched_param param{};
    param.sched_priority = 80;  // out of 1-99; leave headroom above this
    sched_setscheduler(0, SCHED_FIFO, &param);
}
```

Notes:

- `SCHED_FIFO` at a high priority means this thread runs until it
  blocks or yields — a bug that spins forever will starve everything
  else on that core, including the kernel's own housekeeping for that
  core if isolation isn't also in place. Isolate first, elevate
  priority second.
- Call this pinning function *before* opening the multicast socket, so
  the socket's kernel-side processing (soft IRQ, `SO_TIMESTAMPING`
  application) for this thread's traffic is also more likely to run
  affine to the same core, depending on NIC RSS configuration.
- If `DecisionEngine` runs on its own thread rather than inline in the
  receive loop, pin it to a second isolated core and connect the two
  via a lock-free SPSC queue — not a mutex-guarded one, which would
  reintroduce blocking on the hot path.

---

## 5. Reducing other sources of jitter

- **Disable CPU frequency scaling** on the isolated cores —
  `cpupower frequency-set -g performance` (or pin via
  `/sys/devices/system/cpu/cpu<N>/cpufreq/scaling_governor`). A core
  transitioning out of a deep C-state adds microseconds of wake-up
  latency; `performance` governor keeps the core at max frequency and
  out of deep sleep states.
- **Disable transparent huge pages** (`THP`) or set it to `madvise`:
  `echo madvise > /sys/kernel/mm/transparent_hugepage/enabled`. THP's
  background `khugepaged` compaction can preempt a busy-polling thread
  unpredictably; `GapBuffer`'s ~6MB stack-embedded buffer benefits from
  explicit huge pages instead (see below), not THP's opportunistic ones.
- **Explicit huge pages for the receive buffer**, if the socket buffer
  or `GapBuffer`'s storage is moved to a custom allocator: reduces TLB
  misses on the hot path. Not required for correctness — this repo's
  4096-slot buffer is a plain stack array — but worth measuring if
  `tick_to_trade_bench`-style profiling on real hardware shows TLB
  pressure.
- **NIC interrupt coalescing**: `ethtool -C <iface> rx-usecs 0` disables
  coalescing delay so packets are signaled to the kernel as soon as
  they arrive, trading some interrupt-rate overhead for lower latency
  per packet — the right trade for a market-data receive path.
- **Disable `irqbalance`** system-wide if every latency-sensitive NIC
  queue is manually pinned (see §3); leaving it running invites it to
  "helpfully" rebalance an IRQ back onto an isolated core later.

---

## 6. What to verify after tuning, and with what

None of the above is self-verifying — a misconfigured `isolcpus` range
or a driver that ignores `smp_affinity` fails silently. On the real
target host:

- `cat /proc/interrupts` before and after a sustained receive session —
  the isolated cores' interrupt counts for the NIC's queues should not
  move.
- `taskset -pc <pid>` on the running receiver thread to confirm it's
  still on the core it was pinned to (a bug in the pinning code, or a
  `cgroups` CPU quota fighting the affinity mask, can silently override
  it).
- Re-run `tools/timestamp_validate` on the tuned host and compare its
  `kernel_rx_sw_ts -> read` figure against the untuned baseline in
  `BENCHMARK_RESULTS.md` (mean ~1.3µs / p99.9 ~16µs on an idle,
  untuned container). A properly isolated and pinned thread should
  show a materially tighter, more consistent number — that comparison
  is the actual evidence tuning worked, not any individual step above
  in isolation.
- If a supported NIC is available, also re-run with
  `enable_hw_timestamps = true` and confirm
  `SOF_TIMESTAMPING_RAW_HARDWARE` populates — see `BENCHMARK_RESULTS.md`
  §"SO_TIMESTAMPING validation" for why that couldn't be done in this
  repo's own environment.
