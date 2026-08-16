# Measurement 05 — Frame Scheduler Baseline and Candidate Comparison

**Date:** 2026-08-15
**Baseline commit:** `0124d56` (tag `baseline-x86-32`)
**Instrument:** `docs/audit/tools/sched_probe.c` — a standalone replica of the dedicated
frame loop. It is not engine code and links nothing from ReHLDS; it reproduces
`sys_ded.cpp:156-177`, all four `sys_linux.cpp` sleep primitives, `net_ws.cpp:979-1045`
(including both defects), `sys_engine.cpp:107-119` and `host.cpp:663-727`.

**Why a replica rather than the real server:** the scheduler is fully separable from game
content, so this yields Phase 1/2 evidence without a `cstrike` install, and isolates the
loop from engine execution cost. Numbers here describe the *scheduler*, not a live server.
They must be re-confirmed against the real binary under 5v5 load before Phase 2 lands —
see §6.

**Conditions:** i9-12900K, Ubuntu 24.04, kernel 6.17.0-22, `CONFIG_HZ=1000`,
`PREEMPT_VOLUNTARY`. Pinned with `taskset -c 2` (P-core, SMT sibling cpu3 left idle).
5 s per run, otherwise-idle host. **Governor was `powersave`** — see §7, this is the main
caveat on the absolute values.

---

## 1. Headline result

**`sys_ticrate 2000` does nothing at all under `-pingboost` 0, 1 or 2.** It produces a
frame rate statistically identical to `sys_ticrate 1000`, because the loop's fixed ~1 ms
sleep — not the tick rate — is the limiter.

| mode | ticrate | achieved Hz | p50 interval | reject ratio |
|---|---|---|---|---|
| old (`-pingboost` absent/0) | 1000 | 941.5 | 1060.2 µs | 0.000 |
| old | **2000** | **941.4** | **1060.2 µs** | 0.000 |
| select (`-pingboost 2`) | 1000 | 943.2 | 1060.2 µs | 0.000 |
| select | **2000** | **942.3** | **1060.3 µs** | 0.000 |
| timer (`-pingboost 1`) | 1000 | 982.7 | 1015.2 µs | 0.000 |
| timer | **2000** | **984.3** | **1015.1 µs** | 0.000 |

The p50 intervals are identical to the tenth of a microsecond across a 2× change in
`sys_ticrate`. `reject_ratio = 0.000` means `Host_FilterTime` admitted *every* iteration —
the gate is never the binding constraint at these settings; the sleep is.

**This confirms Finding 2.1 of `01-frame-scheduler.md`.** An operator running
`sys_ticrate 2000` without `-pingboost 3` is getting ~941 Hz.

## 2. `-pingboost 3` is the only mode that exceeds ~1000 Hz, and it is expensive

**This confirms Findings 3.1 and 3.2.**

| mode | ticrate | achieved | p50 | p99 | p99.9 | max | CPU | ctx switches | blind-sleep % |
|---|---|---|---|---|---|---|---|---|---|
| net | 100 | 99.2 | 10066.8 | 10209.7 | 10321.9 | 10331.9 | — | — | 81.9 % |
| net | 500 | 484.5 | 2061.1 | 2218.5 | 2715.7 | 3035.0 | — | — | 69.3 % |
| net | 1000 | 941.5 | 1060.5 | 1135.7 | 1667.9 | 2781.2 | 0.8 % | 4 705 | 66.0 % |
| net | **2000** | **1884.2** | **525.4** | 638.5 | 1278.2 | 2315.5 | **3.0 %** | **93 758** | **95.9 %** |

Two things follow:

- At `sys_ticrate 2000` the `(1000 / fps) * 1000` truncation collapses the select timeout
  to 1 µs, so the loop iterates **93 758 times to admit 9 421 frames** (`reject_ratio 0.900`)
  — ten spins per frame, ~18 700 context switches per second, 4× the CPU of mode 0.
  It works, but by brute force.
- **`blind_pct` reaches 95.9 %** — 95.9 % of the `select()` calls pass *no* sockets and
  therefore cannot wake on network input. The stagger logic's `numFrames` counter is
  exhausted almost immediately at high tick rates and stays negative for the rest of each
  1-second window. The "wake early for packets" property the brief assumed exists is, in
  practice, absent 66–96 % of the time **in every mode-3 configuration tested, including
  at `sys_ticrate 100`.**

## 3. Timer slack accounts for the 1000 Hz shortfall

Same code, same mode, only `prctl(PR_SET_TIMERSLACK, 1)` added:

| mode | ticrate | slack | achieved | p50 |
|---|---|---|---|---|
| old | 1000 | 50 µs (default) | 942.6 | 1060.1 µs |
| old | 1000 | 1 ns | **987.2** | **1009.9 µs** |
| old | 2000 | 50 µs | 941.6 | 1060.1 µs |
| old | 2000 | 1 ns | 986.3 | 1010.0 µs |

The p50 moves by **50.2 µs** — the default timer slack, exactly. A one-line `prctl` recovers
~45 Hz at `sys_ticrate 1000` with no other change. It does **not** rescue 2000 Hz, because
the 1 ms quantum still dominates.

Note the asymmetry: with a *relative* sleep the slack is added every iteration and lands in
p50 as systematic drift. With an *absolute* deadline (§4) it cannot accumulate — a late
wakeup does not move the next deadline — so it appears only in the tail. For the absolute
modes the slack reduction was **within run-to-run noise** at this sample size and should
not be claimed as a win there.

## 4. Candidate: absolute monotonic deadline

`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, …)`, with the deadline replacing
`Host_FilterTime`'s backward-looking gate rather than being stacked on top of it.

> Composing the two was tried first and is instructive: an absolute sleep to a deadline the
> gate then rejects produces an immediate-return busy-spin (1.2 M iterations in 5 s). The
> deadline and the admission gate **cannot coexist** — the deadline must *become* the
> admission decision. This is direct evidence for design point 2 in `01-frame-scheduler.md`.

### At `sys_ticrate 2000`, idle

| approach | achieved | p50 | p95 | p99 | p99.9 | max | CPU | ctx switches |
|---|---|---|---|---|---|---|---|---|
| pingboost 0/1/2 | 941 | 1060.2 | 1068.7 | 1126.2 | 1690.4 | 2351.3 | 0.7 % | 4 710 |
| pingboost 3 | 1884.2 | 525.4 | 544.2 | 638.5 | 1278.2 | 2315.5 | 3.0 % | 93 758 |
| **absdeadline** | **1991.1** | **500.0** | 515.6 | 600.2 | 1383.5 | 2595.5 | **1.4 %** | **9 956** |
| **absspin (50 µs)** | **1996.9** | **500.0** | **500.5** | **511.1** | **953.6** | **1927.1** | 9.6 % | 9 981 |

### At `sys_ticrate 1000`, idle

| approach | achieved | p50 | p99 | p99.9 | CPU | ctx switches |
|---|---|---|---|---|---|---|
| pingboost 0 | 941.6 | 1060.2 | 1135.0 | 1990.0 | 0.7 % | 4 709 |
| pingboost 1 | 982.7 | 1015.2 | 1181.9 | 1819.1 | — | — |
| **absdeadline** | **999.2** | **1000.0** | 1079.3 | 1722.0 | 0.7 % | 4 996 |
| **absspin (50 µs)** | 999.0 | 1000.0 | **1030.9** | 1860.0 | 4.9 % | 4 996 |

**`absdeadline` dominates `pingboost 3` on every axis at once:** higher achieved rate
(1991 vs 1884), exact p50, better p99, **less than half the CPU**, and **9.4× fewer context
switches**. This is not a trade-off — it is strictly better, because mode 3 spends its CPU
spinning through `select()` rather than waiting accurately.

**`absspin` is a genuine trade-off**, and should be an opt-in cvar, not a default: it buys
p99 511 µs vs 600 µs and p99.9 954 µs vs 1384 µs, for ~7× the idle CPU (9.6 % vs 1.4 % of a
core). Worth it for a competitive match server, wasteful for a public one.

## 5. Under simulated frame load

`--work-us` adds a busy-work block to each admitted frame, modelling engine execution cost.
At `sys_ticrate 2000` (500 µs period):

| work | mode | achieved | p50 | p99 | p99.9 | CPU | ctx switches |
|---|---|---|---|---|---|---|---|
| 100 µs | net | 1887.4 | 520.7 | 669.2 | 1335.0 | 21.3 % | 77 949 |
| 100 µs | **absdeadline** | **1991.9** | **500.0** | **546.3** | **1054.6** | 21.2 % | **9 960** |
| 200 µs | net | 1906.5 | 515.3 | 559.6 | 1539.1 | 39.9 % | 59 092 |
| 200 µs | **absdeadline** | **1987.9** | **500.0** | 553.9 | **1221.4** | 40.7 % | **9 939** |
| 300 µs | net | 1934.3 | 510.3 | 580.6 | 1029.9 | 59.2 % | 39 515 |
| 300 µs | **absdeadline** | **1986.7** | **500.0** | 601.9 | **907.7** | 59.9 % | **9 933** |

The advantage holds under load: same CPU, 4–8× fewer context switches, exact p50, and the
tail is better in five of six comparisons. Mode 3's spin count naturally falls as work
grows (there is less idle time to spin through), which is why its context-switch count
*drops* from 78 k to 40 k as load rises — its cost is highest precisely when the server is
quietest.

`missed_deadlines` for `absdeadline` rises with load (22 → 34 → 44 per 5 s) — this is the
counter that should become a server-visible statistic, since it is the honest signal that a
configured tick rate is unsustainable.

## 6. What this does *not* establish

Stated plainly, because these numbers are persuasive and could easily be over-read:

- **No engine code was measured.** This is a loop replica. Real `SV_Frame` cost, snapshot
  clustering, plugin callbacks and GC-like pauses are absent. The p99.9 of a real server
  will be worse, and possibly dominated by engine work rather than scheduling.
- **No network traffic.** The socket exists but nothing sends to it, so the socket-wait
  path is never actually woken by a packet. `blind_pct` measures which *branch* was taken,
  not the latency consequence of taking it.
- **Idle host, single pinned thread.** No competing load, no NIC interrupts, no other
  server instances.
- **`powersave` governor** (see §7) — absolute values will shift under `performance`.
- 5-second runs. Adequate for p50/p95/p99; **thin for p99.9 and meaningless for max.** The
  30-minute measured runs the brief specifies (§8.1) have not been done.

The *relative* ordering of the approaches is robust across every configuration tested. The
*absolute* microsecond values are provisional.

## 7. Environment caveat and next step

Runs were made with the governor at `powersave`, because changing it requires root. For the
formal baseline the following should be set and recorded:

```bash
sudo cpupower frequency-set -g performance
```

Re-running §1–§5 under `performance` is the first item of the next measurement pass, along
with 30-minute runs for credible p99.9 and `perf stat` counters for migrations.

## 8. Conclusions for Phase 2

1. **Fixing `NET_Sleep_Timeout`'s arithmetic alone is not the answer.** Even with a correct
   timeout, mode 3 still spins and still blind-sleeps past sockets 96 % of the time. The
   defect is architectural, not arithmetic.
2. **The absolute deadline must replace `Host_FilterTime`'s gate**, not supplement it —
   proven by the busy-spin that results from stacking them.
3. **The launcher's hardcoded `sys->Sleep(1)`** (`sys_ded.cpp:159`) is the root cause of the
   1 ms quantisation and must become tick-rate aware, or frame pacing must move into the
   engine entirely.
4. **`prctl(PR_SET_TIMERSLACK, 1)` is a free ~45 Hz** at `sys_ticrate 1000` and should be
   applied regardless of which scheduler is chosen.
5. **Socket wait and deadline wait must be unified** — likely `ppoll`/`epoll_pwait` with a
   timeout derived from the absolute deadline, or a `timerfd` armed with `TFD_TIMER_ABSTIME`
   in the same poll set. `sched_probe` does not yet model this; it is the next thing to add.
6. **Expose `missed_deadlines` and the interval distribution as server statistics.** The
   legacy FPS counter cannot distinguish 1884 Hz-with-93k-context-switches from
   1991 Hz-with-10k, and that distinction is the entire point of this project.
7. **`sys_ticrate 2000` should warn or refuse** under pingboost 0/1/2, where it is silently
   inoperative. Operators are currently being misled by their own configuration.
