# Measurement 06 — Real-Server Validation of the Scheduler Findings

**Date:** 2026-08-15
**Build:** `audit/m0-baseline` + frame instrumentation, GCC 13.3.0, `-O3`,
`USE_VENDORED_COMPAT_LIBS=OFF`, `USE_STATIC_LIBSTDC=ON`
**Server:** HLDS app 90 + `cstrike` via steamcmd (836 MB), `de_dust2`, **empty server**,
`-insecure -nomaster`, pinned `taskset -c 2` (P-core, sibling idle), governor `powersave`
**Instrument:** `sv_rehlds_perf_frame 1` + `rehlds_perf_frame_dump` (this commit)

This closes the loop on `05-scheduler-measurements.md`: those numbers came from a standalone
replica of the loop, these come from the real engine binary running real Counter-Strike.

---

## 1. The model was right

| | standalone replica | real engine | delta |
|---|---|---|---|
| achieved Hz @ ticrate 1000 | 941.6 | 928.8 | −12.8 |
| p50 frame interval @ 1000 | 1060.2 µs | 1070.9 µs | +10.7 µs |

The real server is ~11 µs per frame slower, and the instrumentation says why: measured
**frame execution p50 = 8.1 µs** on an empty server. The replica omitted engine work; adding
it back accounts for essentially the whole gap. The replica is a valid instrument for
scheduler experiments.

## 2. Headline finding confirmed on the real binary

`sys_ticrate 2000` without `-pingboost 3` is **inoperative**:

| config | achieved Hz | p50 interval | p99 | p99.9 | reject ratio | overruns |
|---|---|---|---|---|---|---|
| ticrate 1000, default pingboost | 928.8 | 1070.9 µs | 1235.9 | 1959.9 | 0.000 | 626 / 8192 |
| **ticrate 2000, default pingboost** | **927.6** | **1071.2 µs** | 1257.0 | 2010.2 | 0.000 | **8192 / 8192** |
| ticrate 1000, `-pingboost 3` | 928.1 | 1071.7 µs | 1233.2 | 2031.2 | 0.000 | 631 / 8192 |
| ticrate 2000, `-pingboost 3` | 1880.3 | 530.2 µs | 635.9 | 1743.6 | **0.898** | 492 / 8192 |

Doubling `sys_ticrate` from 1000 to 2000 under the default pingboost moves the achieved rate
by **−1.2 Hz** and the p50 interval by **+0.3 µs**. Both are noise. The server is doing
exactly what it did at 1000.

**Every single frame overruns** at ticrate 2000 default — 8192 of 8192 — because each frame
finishes ~1071 µs into a 500 µs period. That counter alone would have surfaced this
configuration error to an operator; nothing in the stock server reports it.

### `-pingboost 3` only matters above 1000

At ticrate 1000, pingboost 3 is **identical** to the default (928.1 vs 928.8 Hz). This is
exactly what the arithmetic in `01-frame-scheduler.md` §3.1 predicts: at `fps = 1000`,
`(1000 / fps) * 1000` evaluates to 1000 µs — the same as the launcher's hardcoded 1 ms
sleep. The truncation defect only bites above 1000, where the expression collapses to 1 µs.

At ticrate 2000 the spin is visible in the engine's own counters: **8.87 rejected loop
iterations per admitted frame**, `reject_ratio 0.898` — against the replica's predicted
0.900. The two instruments agree to within 0.2 %.

## 3. Frame execution is not the constraint at this load

| config | exec p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| ticrate 1000, default | 8.1 µs | 45.7 | 71.0 | 138.9 | 217.4 |
| ticrate 2000, pingboost 3 | 1.3 µs | 9.9 | 33.7 | 60.7 | 193.9 |

On an empty `de_dust2`, an admitted frame costs **8.1 µs against a 1000 µs budget** — under
1 % CPU. The scheduler accounts for essentially all of the shortfall. This is important for
sequencing: **hot-path optimisation (Phase 4) has almost nothing to bite on until there is
real player load**, whereas the scheduler problem is fully visible today.

The p50 drops to 1.3 µs under pingboost 3 at 2000 Hz simply because twice as many frames
each advance the simulation half as far.

Deadline lateness p50 of 599 µs at ticrate 1000 quantifies the drift directly: the average
frame starts more than half a period after the deadline it should have hit.

## 4. Running a modern ReHLDS build against a stock steamcmd HLDS

Three setup obstacles, none of them ReHLDS defects, all of which will recur:

1. **`steamclient.so` is not in the app-90 install.** The engine `dlopen`s it by bare name
   and dies with `FATAL ERROR (shutting down): Unable to initialize Steam` even with
   `-insecure`. Fix: symlink it from the steamcmd distribution and make it findable.
   ```bash
   ln -sf /home/dan/projects/steamcmd/linux32/steamclient.so ~/.steam/sdk32/steamclient.so
   ln -sf /home/dan/projects/steamcmd/linux32/steamclient.so /home/dan/projects/hlds/steamclient.so
   export LD_LIBRARY_PATH="/home/dan/projects/steamcmd/linux32:$LD_LIBRARY_PATH"
   ```
2. **HLDS bundles an ancient `libstdc++.so.6`** (max `GLIBCXX_3.4.16`) and the engine is
   linked with `RUNPATH $ORIGIN/.`, so it loads the bundled one in preference to the
   system's. A GCC 13 build needs `GLIBCXX_3.4.29` and fails with
   `Unable to load engine, image is corrupt`. Fix: build with the existing upstream option
   **`-DUSE_STATIC_LIBSTDC=ON`**, which removes `libstdc++.so.6` from `NEEDED` entirely.
   This is not a workaround — it is what the option exists for.
3. `[S_API FAIL] SteamAPI_Init() failed` is emitted and is **benign** for `-insecure -nomaster`
   LAN testing; the server proceeds and reports `VAC secure mode disabled`.

Full working invocation:
```bash
cd /home/dan/projects/hlds
LD_LIBRARY_PATH="/home/dan/projects/steamcmd/linux32:$LD_LIBRARY_PATH" \
  taskset -c 2 ./hlds_linux -game cstrike -console -nomaster -insecure \
  +sv_lan 1 +map de_dust2 +sys_ticrate 1000 -port 27500
```
Stock binaries are preserved alongside as `engine_i486.so.stock`, `filesystem_stdio.so.stock`,
`hlds_linux.stock`.

## 5. Limits of this run

- **Empty server.** No players, no bots, no traffic, no plugins. Frame execution under 5v5
  load is unmeasured and is the next thing needed.
- `powersave` governor, 8.8-second sample windows (8192-sample ring at ~930 Hz). Adequate
  for p50–p99; **thin for p99.9, and `max` is not meaningful** at this sample count.
- Single map, single run per configuration — no repeat-run variance established.
- The deadline reported is synthetic (the engine schedules against nothing), so "lateness"
  measures drift from an ideal, not a missed commitment the engine ever made.

## 6. What this changes

1. **`sys_ticrate` above 1000 should warn under pingboost 0/1/2.** It is silently
   inoperative, and operators are configuring something that does nothing. This is a
   small, self-contained change worth making early.
2. **Overrun count and interval percentiles should be server-visible**, not just available
   through a debug command. The 8192/8192 overrun signal identifies a misconfigured server
   instantly; the legacy FPS counter does not.
3. **Phase 4 (hot-path optimisation) should wait for load.** At 8 µs of frame execution per
   1000 µs period, there is no bottleneck to profile yet. Phase 2 (scheduler) is where the
   entire measurable deficit is today.
4. The replica in `docs/audit/tools/sched_probe.c` is validated and can be used for fast
   scheduler iteration without a full server cycle.
