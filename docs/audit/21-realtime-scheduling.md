# Measurement 21 — The p99 Gap Closes: SCHED_FIFO + C-state Capping

**Date:** 2026-08-16
**Setup:** i9-12900K, governor `performance`, `de_dust2`, 10 zBots, `-pingboost 4`,
pinned `taskset -c 2` (P-core, SMT sibling idle), `sv_rehlds_sched_spin_us 0`
**Privilege:** `setcap cap_sys_nice+ep` on the launcher; `chmod 666 /dev/cpu_dma_latency`;
`kernel.sched_rt_runtime_us = -1`

This closes the question left open since `07-deadline-scheduler.md`: the deadline scheduler
made p50 and p95 essentially exact, but **p99 sat ~27 % over target and nothing in userspace
could move it**.

---

## 1. Results at `sys_ticrate 1000` (target 1000 µs)

| config | achieved | p50 | p95 | p99 | p99.9 | max | lateness p99 | lateness p99.9 | CPU |
|---|---|---|---|---|---|---|---|---|---|
| A baseline | 998.0 | 999.9 | 1015.4 | 1271.3 | 2445.6 | 3133.5 | 275.7 | 1448.3 | 1.6 % |
| B `-rtprio 50` | 1000.0 | 999.7 | 1012.1 | 1021.5 | 1029.0 | 1039.6 | 29.1 | 34.9 | 3.1 % |
| B2 `-rtprio 50` (repeat) | 1000.0 | 999.9 | 1008.4 | 1013.9 | 1023.9 | 1040.1 | 17.5 | 27.4 | 1.4 % |
| C C-state cap only | 997.2 | 999.9 | 1016.3 | 1353.4 | 2615.8 | 3417.0 | 362.5 | 1618.4 | 1.3 % |
| **D `-rtprio 50` + C-state** | **999.9** | **1000.0** | **1003.1** | **1005.2** | **1008.5** | 2002.0 | **8.2** | **11.8** | **1.5 %** |

## 2. Results at `sys_ticrate 2000` (target 500 µs)

| config | achieved | p50 | p95 | p99 | p99.9 | max | lateness p99.9 | CPU |
|---|---|---|---|---|---|---|---|---|
| baseline | 1992.0 | 500.0 | 513.0 | 694.4 | 1463.3 | 2431.6 | 972.4 | 4.1 % |
| **`-rtprio 50` + C-state** | **1999.8** | **500.0** | **502.0** | **504.4** | **512.7** | 1001.1 | **15.0** | **1.9 %** |

## 3. What this means

**The p99 gap is closed.** At 1000 Hz, p99 goes from 27 % over target to **0.5 % over**;
p99.9 from 145 % over to **0.85 % over**. Deadline lateness at p99.9 improves from 1448 µs to
**11.8 µs — 123×**. At 2000 Hz, p99.9 improves 65× and the achieved rate is 1999.8 of 2000.

**It costs nothing.** CPU is 1.5 % versus 1.6 % at 1000 Hz, and at 2000 Hz it *falls* from
4.1 % to 1.9 % — proper scheduling removes wasted rescheduling work rather than adding any.

**Both levers are needed, and neither is sufficient.** This is the most useful structural
finding here:

- **C-state capping alone (C) does nothing.** p99 of 1353 µs is no better than baseline's
  1271 — within noise, if anything worse.
- **`SCHED_FIFO` alone (B/B2) does almost all of it**, taking p99.9 from 2446 µs to ~1024 µs.
- **Together (D) the residue halves again**: lateness p99.9 27.4 → 11.8 µs.

They address different causes. `SCHED_FIFO` fixes *when the kernel decides to run the
thread*; the C-state cap fixes *how long the core takes to wake up once it does*. Fixing only
the scheduling still leaves the wake-up latency, which is why C only helps in combination.

**The spin window is now obsolete.** `08-load-benchmark.md` §4c found that
`sv_rehlds_sched_spin_us 50` bought a much better p95 for 6.6 % CPU — it was a userspace
workaround for the cold-core effect. The C-state cap addresses that cause directly, at no CPU
cost, and D was measured with **spin at 0**. There is no longer a reason to spin. Keep the
default at 0.

## 4. How to run it

```bash
# one-time, on the host
sudo setcap cap_sys_nice+ep /path/to/hlds_linux    # least privilege; no root at run time
sudo sysctl -w kernel.sched_rt_runtime_us=-1       # do not throttle the RT thread
sudo chmod 666 /dev/cpu_dma_latency                # or hold it open as root

# hold the C-state constraint for the server's lifetime (it applies only while the fd is open)
python3 docs/audit/tools/cstate_hold.py 86400 &

# the server
taskset -c 2 ./hlds_linux -game cstrike -pingboost 4 -rtprio 50 +sys_ticrate 1000 ...
```

`-rtprio` fails soft: without the capability it prints the exact `setcap` command and
continues with normal scheduling rather than refusing to start.

## 5. Risks worth stating

- **A `SCHED_FIFO` thread can starve its core.** With `sched_rt_runtime_us = -1` the kernel
  will not throttle it, so a hang or a busy loop wedges that CPU. Pin the server (`taskset`)
  and leave the SMT sibling idle, as measured here. Do not combine RT priority with a large
  `sv_rehlds_sched_spin_us` — §3 says the spin is unnecessary anyway.
- Priority 50 is arbitrary and untuned; it only needs to beat other runnable work on that
  core. It was not compared against other values.
- The C-state constraint is **system-wide**, not per-core, so it raises idle power for the
  whole machine while held.
- `preempt=full` was **not** tested. The runtime switch at
  `/sys/kernel/debug/sched/preempt` is refused by Ubuntu's kernel lockdown even as root; it
  would need a GRUB boot parameter. Given D's numbers there is little headroom left for it to
  recover.

## 6. What this does not establish

- **Bots, not humans.** Frame execution here is bot AI (`18-physics-breakdown.md` §3), and
  real clients would add lag-compensation work that is still entirely unmeasured — bots never
  trigger `SV_SetupMove`. A 10-human server's *execution* profile will differ; there is no
  reason to expect its *scheduling* to.
- **Single runs**, except B which was repeated. The effects are 60–120× so they are far
  outside the run-to-run variance established in `10-gameplay-ab.md`, but the third decimal
  place is not meaningful.
- **One pinned core with an idle SMT sibling**, on an otherwise idle machine. A busy host
  with competing RT work would behave differently.
- Still no gameplay differential testing. This is a scheduling result, not a correctness one.
