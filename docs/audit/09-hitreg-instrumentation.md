# Measurement 09 — Hit-Registration Instrumentation, First Results

**Date:** 2026-08-16
**Build:** `audit/m0-baseline` + `hitreg_stats` module
**Setup:** `de_dust2`, 10 zBots, `bot_difficulty 2`, `sv_unlag 1`, `sys_ticrate 1000`,
`-pingboost 4`, 120 s sample after 45 s warmup

Instrument: `sv_rehlds_perf_hitreg 1`, then `rehlds_perf_hitreg_dump`.

---

## 1. Methodological finding: bots cannot test lag compensation

```
--- hitreg: lag compensation outcomes (0 victim-evaluations) ---
```

**Zero.** Not "mostly skipped" — lag compensation never ran at all across 120 seconds of
10-bot combat.

The cause is the gate `SV_RunCmd` applies before `SV_SetupMove`
(`rehlds/engine/sv_user.cpp:808-809`):

```c
if (!host_client->fakeclient)
    SV_SetupMove(host_client);
```

Every zBot is a fake client, so no bot ever shoots *through* lag compensation. This matches
`03-lag-compensation.md` §6, which noted the guard is symmetric — but the practical
consequence was not obvious until measured:

> **A bot-only server cannot validate, reproduce, or regression-test any lag-compensation
> or historical-hitbox behaviour whatsoever.**

This directly blocks the `EF_NOINTERP` hypothesis (`03-lag-compensation.md` §3) and every
other item in that document's §7 test plan. Those predictions concern victims of a
*compensated* shooter, and a compensated shooter requires a real client.

**What this changes:** the hitbox workstream needs at least one non-fake client that issues
`usercmd`s and fires. Options, in increasing cost:

1. A real CS 1.6 client driven manually — impossible on this headless host, and not
   repeatable, so unsuitable as a regression gate regardless.
2. A synthetic Protocol 48 client: challenge/connect handshake, netchan sequencing, and
   `clc_move` with delta-compressed usercmds. This is the "scripted clients for
   repeatability" the brief calls for in §8, and it is the only option that yields a
   *deterministic* hitbox regression test. Substantial work — several hundred lines and it
   must satisfy the engine's own validation — but it is the correct foundation.
3. An engine-side synthetic harness that builds a fake client frame history and calls
   `SV_SetupMove` directly. Cheaper, but it tests the function in isolation rather than the
   real path, and `SV_SetupMove` reads a lot of global state.

Option 2 is the recommendation. It is a prerequisite for the entire Phase 3 test matrix,
not an optional extra, and it should be scoped as its own piece of work.

## 2. The studio bone/hull cache is consistently ineffective (~5–16 %)

First run:
```
--- hitreg: studio hull construction (1082) ---
  cache hit=66 miss=1016  hit_ratio=6.10%
```

**Corrected by the repeat runs in `10-gameplay-ab.md`:** across four runs the ratio was
4.87 %, 13.72 %, 16.21 % and 10.62 %. The single-run "6.1 %" figure was over-precise. The
claim that survives is weaker but holds: the hit ratio is **consistently low and highly
variable**, so the great majority of studio hull constructions run full bone setup.

**This is explained by the cache key, and it means the cache is not what it looks like.**
The key (`r_studio.cpp:31-44`) includes `origin` and `frame`. Both change continuously for
any moving, animating player, so two traces against the same player at different moments can
essentially never share a key. A hit requires the *same player in the same state traced more
than once*, which in practice means **multiple bullets resolved within a single frame** —
shotgun pellets, or a burst that lands in one tick.

So `r_studiocache` is not a general-purpose cache; it is a **same-tick memoiser for
multi-pellet weapons**. Its 6 % hit rate against mostly-rifle bot combat is close to the
expected behaviour, not a defect.

**This corrects a hypothesis in the project brief.** §29.10 suggests "cache key quality" as
an optimisation candidate and warns that "a poor hit ratio may be more important than
arithmetic speed." The measurement says otherwise: the hit ratio is low *by construction*,
and no achievable key change would raise it without also making the cache return bones for
the wrong state. Improving this hit ratio is **not** a useful optimisation goal. If studio
bone setup turns out to be a real cost, the answer is to make bone setup cheaper, not to
cache it harder.

Caveats: 1082 constructions is a modest sample, one map, one bot difficulty, and bots use a
narrower weapon mix than humans. A shotgun-heavy or higher-fire-rate scenario should raise
the ratio; if it does not, the model above is wrong.

## 3. Hitgroup distribution — a flagged asymmetry that turned out to be noise

> **RETRACTED.** The asymmetry described below is **not real**. Repeat runs
> (`10-gameplay-ab.md`) show the left/right leg ratio flipping direction between identical
> runs — 50/50, 30/89, 53/38, 23/39. There is no systematic left/right bias; this was
> bot-behaviour variance in a single 354-hit sample. The original text is kept below so the
> reasoning and its correction stay on the record.
>
> **Lesson carried forward:** every gameplay statistic gathered from bots needs a
> within-condition repeat before it is interpreted at all. A single run is not evidence.

```
--- hitreg: player hits by hitgroup (354) ---
  head          22    6.21%
  chest         75   21.19%
  stomach       38   10.73%
  left arm      44   12.43%
  right arm     29    8.19%
  left leg     116   32.77%
  right leg     30    8.47%
```

**Left leg receives 3.9× as many hits as right leg** (116 vs 30), and left arm 1.5× right arm
(44 vs 29). Chest and stomach look unremarkable.

This is **not** a conclusion — it is a flagged anomaly with several innocent explanations
that have not been ruled out:

- Bot approach geometry on `de_dust2` is not left/right symmetric; bots may systematically
  expose one side.
- Player model hitbox volumes for left and right legs may genuinely differ in the studio
  model.
- Bot aim converges on a target point and spreads downward-left with recoil.
- 354 hits is a small sample and this is a single run.

It is recorded because a systematic left/right hitbox asymmetry would be a real
hit-registration defect if the innocent explanations fail. Testing it properly needs a
controlled shot harness — i.e. §1 option 2 again — firing a known ray at a known pose and
checking which hitgroup answers. Two cheap prior steps: dump the hitbox definitions from the
player model and compare left/right volumes, and re-run on a symmetric map.

## 4. What the instrument covers, and what it does not

Counted:
- Lag-compensation outcome per victim-evaluation, split by *reason* for exclusion
  (`rewound`, `unchanged`, dead, `EF_NOINTERP`, teleport, absent from scanned frames).
- Studio hull constructions, split by cache hit/miss.
- Player-resolving tracelines by hitgroup.

Not counted, deliberately: the bullet/knife/flash distinction. Those live in GameDLL-private
`trace_flags` bits — ReGameDLL reserves `BIT(16)` upward for them and explicitly leaves the
low bits to the engine (`regamedll/common/const.h:99-101`) — so the engine has no business
interpreting them. The hitgroup counter therefore includes any traceline that resolves
against a player, not only weapon fire.

Not yet built: per-shot records (the brief's §6.3 `shot_id` ring with target time, bracketing
snapshots, interpolation fraction, and live-vs-historical pose). That is deliberately
deferred until §1 is resolved, because with bots there is nothing for it to record.
