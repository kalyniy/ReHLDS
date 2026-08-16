# Phase 2 — Absolute-Deadline Frame Scheduler (`-pingboost 4`)

**Date:** 2026-08-15
**Branch:** `audit/m0-baseline`
**Conditions:** i9-12900K, Ubuntu 24.04, kernel 6.17.0-22, governor **`performance`**,
pinned `taskset -c 2` (P-core, SMT sibling idle), HLDS + `cstrike`, `de_dust2`,
**empty server**, 8192-sample windows.

---

## 1. What was built

A new opt-in pingboost mode that paces the host loop against an **absolute monotonic
deadline** instead of a relative sleep.

- `NET_Sleep_Deadline()` — `rehlds/engine/net_ws.cpp`, exported via `version_script.lds`.
- `Sleep_Deadline()` / `case 4:` — `rehlds/dedicated/src/sys_linux.cpp`, resolving the new
  export by name and falling back with a warning if the engine predates it.
- `sv_rehlds_sched_spin_us` — optional busy guard window, **default 0** (pure sleep).
- `prctl(PR_SET_TIMERSLACK, 1)` applied on first use.

**Modes 0–3 are untouched.** The change is additive and opt-in.

## 2. The first attempt failed, and the failure is the design evidence

The initial implementation sleeps to the deadline and leaves `Host_FilterTime` alone, on the
reasoning that a full period always elapses between frames so the gate would never reject.

Measured: **769.0 Hz** at `sys_ticrate 1000` — *worse* than the 928.8 Hz of the mode it was
meant to replace — with `reject_ratio 0.206` and a bimodal p95 of 2000.3 µs.

The reasoning was wrong in a specific and instructive way:

> An absolute deadline **self-corrects**. If one wake lands 30 µs late, the *next* interval
> is deliberately ~30 µs **shorter** than a period, so that phase is restored.
> `Host_FilterTime` rejects any interval below `1/(fps+1)` — and at `sys_ticrate 2000` that
> threshold is 499.75 µs against a 500 µs period, a margin of **0.25 µs**.

So the gate rejects precisely the corrections the deadline exists to make, and because the
deadline has already advanced, each rejection costs a whole period. The two mechanisms are
not merely redundant — they are **actively opposed**.

The fix is `g_bSchedDeadlineActive`: when the deadline scheduler is pacing, `Host_FilterTime`
skips its minimum-elapsed check. The deadline *becomes* the admission decision. This
confirms, on the real engine, what `05-scheduler-measurements.md` §4 inferred from the
replica — and it is a stronger result, because here it was a real regression rather than a
harness artefact.

## 3. Results — real server, all four configurations

| config | achieved Hz | p50 interval | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| 1000, `-pingboost` default | 926.8 | 1071.0 µs | 1125.2 | 1315.9 | 1929.9 | 3124.8 |
| **1000, `-pingboost 4`** | **1000.0** | **999.9 µs** | **1011.1** | **1152.7** | **1872.2** | **2309.8** |
| 2000, `-pingboost 3` | 1843.3 | 530.5 µs | 568.2 | 943.8 | 1927.1 | 2631.6 |
| **2000, `-pingboost 4`** | **1995.1** | **499.9 µs** | **510.5** | **660.8** | **1724.1** | **2120.3** |

### Deadline lateness, rejects and overruns

| config | lateness p50 | p95 | p99 | rejected iterations | overruns / 8192 |
|---|---|---|---|---|---|
| 1000, default | 605.3 µs | 1023.7 | 1062.7 | 0 | 645 |
| **1000, `-pingboost 4`** | **12.0 µs** | **21.8** | **174.6** | 0 | **12** |
| 2000, `-pingboost 3` | 277.6 µs | 500.7 | 723.7 | **321 221** | 633 |
| **2000, `-pingboost 4`** | **11.4 µs** | **18.3** | **295.7** | **0** | **66** |

Lateness p50 improves by **50×** at 1000 Hz (605 → 12 µs) and **24×** at 2000 Hz
(278 → 11 µs). Overruns fall by 54× and 10× respectively. `-pingboost 3` performs 321,221
rejected loop iterations in the sample window — 8.58 per admitted frame — where mode 4
performs zero.

> **Note on the lateness column.** It only became meaningful once the instrument was taught
> to read the scheduler's *real* deadline (`g_SchedFrameDeadlineNs`). Before that, frameperf
> compared against its own independently-anchored synthetic deadline, which under mode 4
> measured the phase offset between two unsynchronised clocks and reported a spurious
> `admitted_early=8182`. The earlier synthetic figures for modes 0–3 remain valid, because
> those schedulers target no deadline at all — any reference phase is equally arbitrary.

### CPU and context switches — 20 s steady state, empty server

| config | CPU | context switches / s |
|---|---|---|
| 1000, default | 2.3 % | 1 128 |
| 1000, `-pingboost 4` | 2.6 % | 1 210 |
| 2000, `-pingboost 3` | 5.5 % | **18 286** |
| **2000, `-pingboost 4`** | **3.8 %** | **2 205** |

At 2000 Hz mode 4 uses **31 % less CPU** and **8.3× fewer context switches** than mode 3
while delivering 8 % more frames. Context switches track the frame rate almost exactly
(~1.1 per frame), which is what a correctly sleeping scheduler should look like; mode 3's
9.2 per frame is the spin.

At 1000 Hz mode 4 costs 13 % more CPU but delivers 7.9 % more frames — per-frame cost is
essentially unchanged, and it actually reaches the configured tick rate.

## 4. Deliberate omission: no socket wait

`NET_Sleep_Timeout` selects on the game sockets so it can wake early on a packet. The new
mode does **not**, and this is deliberate rather than an oversight:

A frame rejected by `Host_FilterTime` returns from `_Host_Frame` *before* `SV_Frame` runs,
so **no packet is read on a rejected iteration**. Waking early for a packet therefore cannot
reduce command-to-simulation latency in the current architecture — it only burns CPU. This
also explains why the 66–96 % blind-sleep fraction measured in `05-scheduler-measurements.md`
§2 does so little observable harm.

Making early wakeups useful requires moving packet reading ahead of the admission gate. That
is a substantially larger change with real ordering implications for `SV_ReadPackets`,
`SV_ParseMove` and time-base establishment, and it is **not** attempted here. Recorded as
the next scheduler question rather than silently assumed away.

## 5. What is not established

- **Empty server.** No players, no bots, no traffic, no plugins. Frame execution p50 is
  8.1 µs against a 1000 µs budget, so this measures the scheduler in isolation. Behaviour
  under 5v5 load is the next thing needed, and could change the tail conclusions.
- **No gameplay validation.** The server starts, loads maps and runs; no client has
  connected, and no movement, shooting or round logic has been exercised against this
  build. Bypassing `Host_FilterTime`'s gate changes *when* frames run, so demo/gameplay
  differential testing against the `baseline-x86-32` reference is required before this
  mode could be recommended for production.
- 8-second windows: adequate for p50–p99, thin for p99.9, and `max` is not meaningful.
- Single run per configuration; no repeat-run variance established.
- Linux only. The Windows path forwards to the legacy timeout, since the pingboost
  mechanism lives in the Linux launcher.
- `sv_rehlds_sched_spin_us` is implemented but **unmeasured on the real server**. The
  replica suggested a spin buys a tighter p99 for ~7× idle CPU; that trade has not been
  re-confirmed here, so the default remains 0.

## 6. Next

1. Repeat under real player load — bots or scripted clients — before drawing tail-latency
   conclusions.
2. Differential-test gameplay against `baseline-x86-32`. The gate bypass is the risk.
3. Warn when `sys_ticrate > 1000` under modes 0–2, where it is silently inoperative.
4. Investigate moving packet reading ahead of the admission gate, which is the only way
   early socket wakeup becomes useful (§4).
5. Measure `sv_rehlds_sched_spin_us` on the real server.
