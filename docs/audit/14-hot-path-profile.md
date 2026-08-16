# Measurement 14 — Ranked Hot Path, and the 1000 → 2000 Hz Cost

**Date:** 2026-08-16
**Setup:** `de_dust2`, 10 zBots, `bot_difficulty 2`, `-pingboost 4`, governor `performance`,
pinned `taskset -c 2`, 40 s warmup, 25 s window
**Instrument:** in-engine subsystem timers around `SV_ReadPackets`, `SV_Physics` and
`SV_SendClientMessages` in `SV_Frame_Internal` (`sv_main.cpp:8122`)

`perf(1)` is unusable on this host — `perf_event_paranoid = 4` blocks unprivileged use and
lowering it needs root. The in-engine timers need no privileges and attribute cost to engine
*stages* rather than symbols, which is what the brief's §11 ranking actually asks for.

---

## 1. Ranked hot path at `sys_ticrate 1000`

Frame execution p50 is 19.9 µs against a 1000 µs budget.

| stage | p50 | p95 | p99 | p99.9 | max | share of wall |
|---|---|---|---|---|---|---|
| **`SV_Physics`** | **13.7 µs** | **58.4** | **118.1** | **243.7** | **380.7** | **2.00 %** |
| `SV_ReadPackets` | 1.7 µs | 3.6 | 5.9 | 7.3 | 9.2 | 0.20 % |
| `SV_SendClientMessages` | 1.2 µs | 1.6 | 2.6 | 5.5 | 7.4 | 0.10 % |
| *(whole frame)* | *19.9 µs* | *75.7* | *147.6* | *282.6* | *535.4* | — |

**`SV_Physics` is 69 % of frame execution at p50 and owns the tail** — its p99 of 118.1 µs
against the frame's 147.6 µs, and p99.9 of 243.7 against 282.6, mean essentially every slow
frame is a slow physics frame.

This confirms the brief's §29.8 hypothesis directly. It also settles the ranking question in
§11: snapshot/delta encoding and the command path are **not** where the time goes at this
load. `SV_SendClientMessages` at 1.2 µs p50 and 0.1 % of wall time is not worth optimising,
and the delta JIT it contains is therefore not worth preserving on performance grounds —
which further de-risks disabling it for the x86-64/ARM64 port.

## 2. The 1000 → 2000 Hz cost, measured

| | 1000 Hz | 2000 Hz | change |
|---|---|---|---|
| achieved | 999.1 Hz | 1991.5 Hz | ×1.99 |
| `SV_Physics` p50 | 13.7 µs | 11.9 µs | **−13 %** |
| `SV_Physics` p99 | 118.1 µs | 110.3 µs | −7 % |
| `SV_Physics` share of wall | 2.00 % | **2.80 %** | **×1.40** |
| frame execution p50 | 19.9 µs | 16.7 µs | −16 % |
| frame execution p99.9 | 282.6 µs | 240.2 µs | −15 % |
| process CPU | 4.1 % | 5.6 % | **+37 %** |

**Per-frame physics cost falls only 13 % when the frame rate doubles.** If physics work were
proportional to elapsed simulation time it would halve. It does not, because `SV_Physics`
iterates every non-player edict every admitted frame regardless of `frametime` — the
per-edict scan is fixed cost, and only the integration within it scales.

Net: **doubling the tick rate costs ~40 % more physics work per second and ~37 % more CPU.**

## 3. Go/no-go input for the 1000 → 2000 gate

The brief's risk register anticipated that 2000 Hz "increases `SV_Physics`/`StartFrame` load
enough to worsen tail latency." On this host, at this load, **the first half is confirmed
and the second half is not**:

- Load does rise materially (§2).
- But the tail did **not** degrade — frame execution p99.9 improved (240.2 vs 282.6 µs), and
  interval p99.9 was comparable (`08-load-benchmark.md`).
- Absolute cost remains small: 5.6 % of one pinned core.

So on an i9-12900K with 10 bots on `de_dust2`, 2000 Hz is affordable. That conclusion is
**load-dependent and should not be generalised**: physics cost scales with entity count, so
a server with many grenades, breakables, dropped weapons or hostages will see the 2.8 %
figure rise, and it rises twice as fast at 2000 Hz. The gate should be re-evaluated against
the intended production entity load, not this one.

## 4. Where to look inside `SV_Physics`, if optimisation is ever justified

Not yet justified — 2 % of wall time is not a bottleneck, and the brief forbids optimising
unmeasured paths. Recording the candidates so the next person does not have to re-derive
them:

- The per-edict loop visits every entity each frame; a large fraction are inert
  (`MOVETYPE_NONE`, no think due). Counting how many edicts are visited versus actually
  simulated would show whether the fixed scan cost dominates, which §2's −13 % strongly
  suggests it does.
- `pfnStartFrame` is called once per admitted frame and is GameDLL code; its share is
  currently folded into `SV_Physics` and is not separately measured.
- The p99/p50 ratio of 8.6× means physics is bursty — likely think callbacks and collision
  from grenades and moving entities. Attributing by `movetype` would identify it.

The cheapest next instrument would split `SV_Physics` into `pfnStartFrame`, the edict scan,
and per-`movetype` buckets.

## 5. Caveats

- **No real client connected**, so `SV_SetupMove` never ran and lag compensation contributed
  nothing to `SV_ReadPackets`. That stage's 1.7 µs is therefore a **lower bound** — with
  real clients it includes the rewind loop over all players. Given the measured session in
  `13-pose-divergence.md` sustained 30,519 victim evaluations in a few minutes, this is not
  negligible and should be re-measured with clients connected.
- Bot AI cost is charged to the GameDLL inside `pfnStartFrame`/think callbacks and is folded
  into `SV_Physics`. A 10-human server would have different physics cost than 10 bots.
- Single map, single run per configuration, 8192-sample ring covering the last ~8 s of a
  25 s window.
- The three instrumented stages account for ~2.3 % of wall time at 1000 Hz while frame
  execution is ~2.0 %; the small excess is the timers' own overhead plus stages not
  instrumented (`SV_CheckCmdTimes`, `SV_CheckTimeouts`, `Steam_RunFrame`).
