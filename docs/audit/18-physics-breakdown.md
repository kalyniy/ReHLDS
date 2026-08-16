# Measurement 18 — Inside `SV_Physics`: the Tail Is the GameDLL

**Date:** 2026-08-16
**Setup:** `de_dust2`, 10 zBots, `-pingboost 4`, `sys_ticrate 1000`, pinned P-core, three runs
**Instrument:** `FP_SUB_STARTFRAME` timing plus per-frame edict counts in `SV_Physics`

`14-hot-path-profile.md` established that `SV_Physics` is 69 % of frame execution and owns
the tail. This splits it into the GameDLL's `pfnStartFrame` and the engine's own edict walk.

---

## 1. Result

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| `SV_Physics` p50 | 3.3 µs | 12.3 | 12.9 |
| `pfnStartFrame` p50 | 0.7 µs | 1.7 | 1.9 |
| **StartFrame share at p50** | 21 % | 14 % | 15 % |
| `SV_Physics` p99.9 | 196.8 µs | 693.0 | 194.8 |
| `pfnStartFrame` p99.9 | 191.1 µs | 690.1 | 191.8 |
| **StartFrame share at p99.9** | **97.1 %** | **99.6 %** | **98.5 %** |

Edict accounting was near-identical every run: **271 edicts visited per frame, ~239
simulated (88 %)**.

## 2. What it means

**The steady cost and the tail have different owners.**

- At p50, `pfnStartFrame` is only 14–21 % of `SV_Physics`. The bulk of the median cost is the
  engine walking 271 edicts — roughly 38 ns per edict.
- At p99.9, `pfnStartFrame` is **97–99.6 %** of it. Essentially every slow physics frame is a
  slow *GameDLL* frame.

Since tail latency is what this project optimises for, the practical conclusion is blunt:
**optimising the engine's physics loop would not measurably improve p99.9 on this workload.**
The bursts come from game code — here, bot AI.

Note also that the absolute numbers moved 3.5× between runs (p99.9 of 196.8 vs 693.0 µs)
while the *ratio* stayed within 2.5 points. This is the `10-gameplay-ab.md` lesson applying
again: with bots, absolute timings need repeats before they mean anything, but ratios within
a single run are stable and are the safer thing to reason about.

## 3. The caveat that limits this result

**zBot AI runs inside `pfnStartFrame`.** A server with ten humans instead of ten bots does
essentially none of that work, so `pfnStartFrame` would be far cheaper and the balance would
shift back toward the engine's edict walk.

That means the benchmark harness — which has been the basis for every performance number in
this project so far — **systematically overstates GameDLL cost relative to a real 10-human
server**. It is still the right tool for scheduler work, where the load just needs to be
representative and repeatable, but it is the wrong tool for deciding where to optimise
gameplay-path code.

Confirming this needs real clients, which is the same dependency the hitbox work has
(`09-hitreg-instrumentation.md` §1). Until then, "the tail is GameDLL work" should be read as
"the tail is GameDLL work *when bots are playing*."

## 4. The edict walk itself

271 visited, 239 simulated — so only 12 % is spent skipping free slots and player edicts. The
walk is not obviously wasteful in the sense of scanning mostly-dead entities.

But "simulated" here means *reached the movetype switch*, and most of those are
`MOVETYPE_NONE` → `SV_Physics_None` → `SV_RunThink`, which does nothing unless a think is
due. So the 88 % figure should not be read as 88 % of the walk doing meaningful work; it
means the cost is spread thinly across many cheap entities rather than concentrated.

That is consistent with `14-hot-path-profile.md` §2, where doubling the frame rate cut
per-frame physics cost by only 13 %: the walk is a fixed per-frame cost that does not scale
down with `frametime`.

**A per-`movetype` breakdown would be the next refinement** and is deliberately not done
here — at 0.84–1.26 % of wall time, `SV_Physics` is not a bottleneck worth further
instrumentation until something makes it one.

## 5. Revised picture of the hot path

Combining with `14-hot-path-profile.md`:

```
frame execution p50 ~5-19 us  (varies with round phase)
├── SV_Physics            ~65-70% of it
│   ├── edict walk        ~80-85% at p50   <- engine, fixed per-frame cost
│   └── pfnStartFrame     ~15-20% at p50, ~98% at p99.9   <- GameDLL, all the burstiness
├── SV_ReadPackets        ~2-8%   (no lag comp in this measurement - bots only)
└── SV_SendClientMessages ~1-4%
```

Nothing here is a bottleneck at 1000 Hz: the whole frame is ~2 % of a 1000 µs budget. The
value of this measurement is negative — it rules out engine physics optimisation as a way to
improve tail latency, and points at game code instead.
