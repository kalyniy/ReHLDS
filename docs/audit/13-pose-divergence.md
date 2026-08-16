# Measurement 13 — Quantifying the Origin-Only Rewind

**Date:** 2026-08-16
**Setup:** `de_dust2`, 9 zBots + one real LAN client (~5 ms ping), `sv_unlag 1`,
`sv_maxunlag 0.5`, `sys_ticrate 1000`, `-pingboost 4`. Active combat.
**Instrument:** `HitReg_PoseDivergence`, sampled at the moment of rewind in `SV_SetupMove`.

`03-lag-compensation.md` proved from source that `SV_SetupMove` restores `origin` and
nothing else, so the studio hull is built from the victim's **live** pose at their
**historical** position. This is the first measurement of how far apart those two epochs
actually are.

---

## 1. Results

30,519 victim evaluations; 13,208 rewinds; 8,192 sampled for percentiles.

```
--- hitreg: rewound-pose divergence (8192 samples of 13208) ---
  historical snapshot vs LIVE edict at hitbox construction time
  yaw delta      p50=    1.80 p90=   19.60 p99=   44.47 p99.9=   90.09 max=  125.00 deg
  pitch delta    p50=    0.01 p90=    0.32 p99=    3.32 p99.9=    7.79 max=   10.02 deg
  rewind         p50=   37.05 p90=   51.99 p99=   55.00 p99.9=   56.00 max=   59.02 ms
  sequence mismatch=519 (3.93%)  gaitsequence mismatch=959 (7.26%)
```

Lag-compensation outcomes for the same session, consistent with `12-real-client-lagcomp.md`:
rewound 43.28 %, unchanged 46.26 %, dead 10.29 %, `EF_NOINTERP` 0.05 %, teleport 0.12 %.
Studio hull constructions 107,932 with a 7.02 % cache hit ratio — again inside the
4.87–16.21 % band.

## 2. What this shows

**The divergence is real, routine, and large in the tail.**

- Half of all rewinds carry under 2° of yaw error — negligible.
- One rewind in ten carries **~20°**.
- One in a hundred carries **~44°**.
- One in a thousand carries **~90°** — the stored facing is a quarter-turn from the live
  facing that the hitbox will actually be built from.

Pitch barely diverges (p99 = 3.3°), which is expected: players yaw far more than they pitch.

**The rewind window here is only ~37 ms at p50.** This is a LAN client at 5 ms ping — close
to the best case the engine will ever see. Divergence is roughly angular velocity × window,
so a typical internet client at 50–100 ms would see a proportionally wider window and
correspondingly larger errors. **These numbers are a floor, not a typical case.**

Animation state diverges too: the victim is playing a different `sequence` in 3.93 % of
rewinds and a different `gaitsequence` in 7.26 %. Both feed bone setup.

## 3. What this does *not* show — an important limit

**This measures `entity_state_t.angles[1]` against `edict->v.angles[1]`. It does not measure
the quantity that actually orients the hitboxes.**

Under ReGameDLL's blending interface the root bone matrix yaw is overwritten with
`m_flGaityaw` (`regamedll/dlls/animation.cpp:1183`), discarding the yaw the engine passed
in. `m_flGaityaw` is a *smoothed* quantity that converges toward `pev->angles.y` over
several frames (`regamedll/dlls/player.cpp:9034-9040`), and it is neither networked nor
stored in any snapshot — so the engine cannot compare it against history at all.

Two consequences:

1. Because gait yaw **lags** aim yaw, its divergence is plausibly *smaller* than the numbers
   above. The measured yaw delta is a **correlated proxy and probably an overestimate** of
   the true hitbox rotation error. It should not be quoted as "hitboxes are rotated by 44°
   at p99."
2. Measuring the real figure requires per-frame server-side history of `m_flGaityaw`, which
   does not exist today and would have to be added **in ReGameDLL**, not the engine. That is
   why an owner-controlled ReGameDLL fork matters for this workstream.

What the measurement *does* establish beyond doubt is that **the pose state genuinely
diverges during the compensation window** — the rewind is not harmlessly reconstructing an
almost-identical pose. The magnitude of the resulting hitbox error is bounded above by these
figures and remains to be pinned down exactly.

## 4. Why this sampling point was chosen

The obvious instrument is a per-bullet log: intercept each shot, record historical vs live
pose, record the trace result. That yields a handful of samples per engagement.

Sampling at the rewind instead yields **every rewind** — 13,208 in a few minutes of one
client — because `SV_SetupMove` runs on every usercmd, not only when the player fires. It
also needs no coupling to weapon code, so it works identically for any mod.

The trade-off: it measures the *divergence*, not the *outcome*. It cannot say whether a
given shot hit or missed because of it. A per-bullet instrument correlating divergence with
`TraceResult` is still the right next step for establishing gameplay impact — but it should
be built knowing the divergence distribution, which is now known.

## 5. Open questions this raises

1. **What does the divergence do to hit outcomes?** Needs the per-bullet correlation above,
   plus a controlled scenario (fixed shot ray, victim executing a scripted yaw sweep).
2. **How much of the yaw delta survives into `m_flGaityaw`?** Needs ReGameDLL-side history.
   Until then the hitbox error is bounded, not measured.
3. **Does it scale with latency as expected?** Testable now by raising the rewind window —
   `sv_unlagpush`, or a client with artificial latency — and re-running. If divergence does
   not grow roughly linearly with the window, the model is wrong.
4. **Would rewinding the fields that *are* in the snapshot help or hurt?** `angles`,
   `sequence`, `frame`, `blending`, `controller` and `usehull` are all already stored and
   unused. Restoring them while `m_flGaityaw` stays live could produce a *differently* wrong
   pose rather than a correct one — `03-lag-compensation.md` §7.5 flagged this, and these
   numbers do not resolve it. It needs an A/B with the per-bullet instrument.
