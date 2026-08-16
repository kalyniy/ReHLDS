# Measurement 12 — Lag Compensation With a Real Client

**Date:** 2026-08-16
**Setup:** `de_dust2`, 9 zBots + **one real CS 1.6 client** (LAN, ~5 ms ping), `sv_unlag 1`,
`sv_maxunlag 0.5`, `sys_ticrate 1000`, `-pingboost 4`, ~5 minutes connected
**Instrument:** `sv_rehlds_perf_hitreg 1` → `rehlds_perf_hitreg_dump`

This is the first measurement of the lag-compensation path in this project. Bots cannot
produce one — `SV_RunCmd` gates `SV_SetupMove` on `!host_client->fakeclient`
(`sv_user.cpp:808`), so a bot-only server yields exactly zero victim evaluations
(`09-hitreg-instrumentation.md` §1). One real connected client unblocks the whole path,
because `SV_SetupMove` runs on **every** usercmd, not only when the player shoots.

---

## 1. Results

```
--- hitreg: lag compensation outcomes (46328 victim-evaluations) ---
  rewound                                     25429   54.89%
  unchanged                                   16468   35.55%
  skip: dead (health<=0)                       4383    9.46%
  skip: EF_NOINTERP (jump/reload/death)          18    0.04%
  skip: teleport detected                        30    0.06%
  skip: absent from scanned frames                0    0.00%
  --> 9.56% of victim-evaluations were NOT lag compensated
--- hitreg: studio hull construction (5896) ---
  cache hit=470 miss=5426  hit_ratio=7.97%
--- hitreg: player hits by hitgroup (1428) ---
  head                 97    6.79%
  chest               203   14.22%
  stomach             327   22.90%
  left arm            260   18.21%
  right arm           193   13.52%
  left leg            203   14.22%
  right leg           145   10.15%
```

## 2. The `EF_NOINTERP` prediction is refuted

`03-lag-compensation.md` §3 derived from source that any victim carrying `EF_NOINTERP` in
any scanned frame is dropped from compensation entirely, and that ReGameDLL raises that flag
on jump, reload and death. It called this "the most directly falsifiable claim in this
document" and suggested it "may well dominate in practice", with a predicted symptom of
*"shooting a jumping or reloading opponent behaves as if `sv_unlag 0`."*

**Measured: 18 exclusions out of 46,328 evaluations — 0.04 %.**

The mechanism is confirmed to exist — the counter is not zero, so the path does fire — but
it is three orders of magnitude away from mattering. The predicted gameplay symptom would be
undetectable.

The reasoning missed a rate argument. `SV_CleanupEnts` (`sv_main.cpp:4991-4998`) clears
`EF_NOINTERP` from every edict once per server frame. At `sys_ticrate 1000` the flag is
therefore live for roughly one millisecond per animation event, while the frame history that
`SV_SetupMove` scans is sampled at the *snapshot* rate — tens of hertz, not a thousand. The
chance that a transient one-frame flag is captured in a stored snapshot is correspondingly
small. Source inspection established that the flag *disqualifies*; only measurement
established how rarely it is *seen*.

**90.4 % of victim evaluations were compensated.** The dominant exclusion by far is
`health <= 0` at 9.46 %, which is correct — dead players should not be rewound.

## 3. What this does *not* refute

The central finding of `03-lag-compensation.md` is untouched by this measurement:

> The rewind restores `origin` and nothing else. `angles`, `sequence`, `frame`, `blending`,
> `controller` and the duck hull are read live at trace time, as are ReGameDLL's
> `m_flYaw` / `m_flPitch` / `m_iGaitsequence` / `m_flGaityaw` — the last of which supplies
> the **root bone matrix yaw**.

That is a structural property of the code, verified line by line, and this instrument does
not test it. The 54.89 % of evaluations recorded as `rewound` are precisely the cases where
a victim's origin was moved back in time **while their pose stayed live** — i.e. the
chimera. Measuring its *effect* needs the per-shot pose comparison described in
`03-lag-compensation.md` §7.1: historical vs live `angles`/`sequence`/`frame`/`blending`
logged per bullet, alongside the resulting `TraceResult`.

So the state of that document is now:

| claim | status |
|---|---|
| Rewind restores origin only; pose is live | **proven from source, effect unmeasured** |
| Root bone yaw comes from live `m_flGaityaw` | **proven from source, effect unmeasured** |
| Studio cache key omits the gait fields | **proven from source** |
| `EF_NOINTERP` excludes jumping/reloading victims | **mechanism confirmed, magnitude refuted (0.04 %)** |
| Guard asymmetry can leave entities rewound | structural; not observed (0 anomalies seen) |

## 4. Incidental results

**Studio cache hit ratio 7.97 %** (470 / 5896) with a real client, sitting inside the
4.87–16.21 % band from the bot runs. Consistent with the same-tick-memoiser model in
`09-hitreg-instrumentation.md` §2.

**Hitgroups**, 1428 hits: left leg 203 vs right leg 145 (1.4×) and left arm 260 vs right arm
193 (1.35×). Much weaker than the 3.9× that a single bot run threw up and later retracted,
and in the same direction. Not significant on its own, but if a controlled shot harness is
ever built, checking left/right symmetry is cheap to include.

**`skip: absent from scanned frames` is 0** across 46,328 evaluations. The concern in
`03-lag-compensation.md` §6 that victims outside the shooter's `packet_entities` are silently
traced live did not materialise on a 10-slot server on `de_dust2`. It may still matter at
higher player counts or with entity eviction.

## 5. Method note

The refutation only happened because the prediction was written down as a falsifiable
statement with a specific expected magnitude *before* it could be measured. A vaguer claim —
"jumping players may have hitbox issues" — would have been unfalsifiable and would have
survived indefinitely.

Two source-derived predictions in this project have now been checked against measurement.
One (`sys_ticrate 2000` is inoperative under pingboost 0–2) was confirmed almost exactly.
This one was refuted. Source reading is good at establishing *what the code does* and
unreliable at establishing *how often it matters*.
