# Measurement 22 — Real-Client Profile: `perf`, Cache Behaviour, and the Config Fix Validated

**Date:** 2026-08-16
**Setup:** `de_dust2`, **one real LAN client + 9 zBots**, ~15 min of active play (56 frags),
`-pingboost 4`, `sys_ticrate 1000`, pinned `taskset -c 2`, competitive `server.cfg`
(`docs/competitive-server-config.md`) with **`sv_maxupdaterate 102`**
**Tools:** `perf record` / `perf stat` (unblocked by `kernel.perf_event_paranoid=1`), plus the
in-engine instrumentation

This is the first profile taken with a real client, which matters because **bots never
trigger lag compensation** — `SV_RunCmd` gates `SV_SetupMove` on `!host_client->fakeclient`.
Every previous profile had that path entirely absent.

---

## 1. The `sv_maxupdaterate` fix, validated end-to-end

Same player, same map, same weapons — the only change is `sv_maxupdaterate 30 → 102`, which
stops the server overriding the client's `ex_interp 0.01`
(`docs/competitive-server-config.md` §1).

| | before (`30`) | after (`102`) | change |
|---|---|---|---|
| rewind window p50 | 37.05 ms | **17.20 ms** | −54 % |
| rewind window p99 | 55.00 ms | **31.20 ms** | −43 % |
| **yaw divergence p50** | 1.80° | **0.92°** | −49 % |
| **yaw divergence p90** | 19.60° | **10.39°** | −47 % |
| **yaw divergence p99** | 44.47° | **20.91°** | −53 % |
| sequence mismatch | 3.93 % | **1.36 %** | −65 % |
| gaitsequence mismatch | 7.26 % | **3.70 %** | −49 % |

**Halving the rewind window halved the pose error.** That is direct confirmation of the model
in `13-pose-divergence.md` — divergence scales roughly linearly with the compensation window —
and it means **a config change alone cut hit-registration error roughly in half**, with no
code involved.

Caveat: p99.9 and max went the *other* way (90.09° → 103.47°, 125° → 178.67°). With 8192
samples, p99.9 is ~8 observations, and this session involved far more aggressive flicking (56
frags in 15 minutes) than the earlier one. Treat the far tail as noise; p50–p99 moved
consistently and by a lot.

Lag-comp outcomes over **295,385 victim-evaluations** (10× the previous dataset):
rewound 41.49 %, unchanged 45.50 %, dead 12.94 %, **`EF_NOINTERP` 0.04 %**, teleport 0.04 %,
absent 0.00 %. The `EF_NOINTERP` refutation from `12-real-client-lagcomp.md` holds at ten
times the sample size.

## 2. `perf` function profile — collision dominates, and lag comp is cheap

914 samples at 4999 Hz over 75 s of live play. Percentages are of the server's own CPU time,
which is only ~2–4 % of one core.

| % | function | where |
|---|---|---|
| **7.57** | `SV_RecursiveHullCheck` | engine — collision |
| 4.76 | `CNavAreaGrid::GetNearestNavArea` | GameDLL — bot nav |
| 3.83 | `SV_Physics` | engine |
| 3.78 | `SV_CheckMovingGround` | engine — per edict |
| 2.29 | `PM_RecursiveHullCheck` | engine — player movement |
| 2.25 | `VectorMA` | engine — math |
| 2.08 | `SV_PointContents` | engine — collision |
| 1.78 | `SV_LinkEdict` | engine |
| **1.62** | `__x86.get_pc_thunk.ax` | **PIC overhead — see §4** |
| 1.37 | `SV_FindTouchedLeafs` | engine |
| 1.11 | `SV_SendClientMessages` | engine |
| 1.05 | `SV_CheckWater` | engine |
| 0.86 | `SV_WriteEntitiesToClient` | engine |
| **0.83** | **`SV_SetupMove`** | **engine — lag compensation** |

**Two findings.**

**Collision and tracing is the real engine hot cluster.** `SV_RecursiveHullCheck` +
`PM_RecursiveHullCheck` + `SV_PointContents` + `SV_FindTouchedLeafs` + `SV_CheckWater` ≈
**14.4 %**, comfortably the largest engine group and roughly double `SV_Physics` itself. That
is *not* where the earlier subsystem timers pointed, and both are right: the timers measured
the *tail* (which is GameDLL bursts, `18-physics-breakdown.md`), while this measures *steady
cycles*.

**Lag compensation is cheap: `SV_SetupMove` is 0.83 %.** This closes the open question from
`14-hot-path-profile.md` §5, where the 1.7 µs `SV_ReadPackets` figure excluded lag comp
entirely because bots cannot trigger it. Even with a real client rewinding 9 victims per
command, it does not register as a cost. **Scaling to 10 real players would be ~10× the
commands, so on the order of 8 % of a 2–4 % CPU budget — still not a bottleneck.**

## 3. Hardware counters — memory-bound, not compute-bound

60 s under the same load:

| counter | value |
|---|---|
| cycles | 5,828,234,699 |
| instructions | 5,981,549,577 |
| **IPC** | **1.03** |
| cache-references | 120,834,941 |
| **cache-misses** | **38,051,010 — 31.49 %** |
| branch-instructions | 1,139,816,072 |
| branch-misses | 23,583,394 — 2.07 % |

**IPC of 1.03 on a core capable of 4+, with a 31.5 % cache-miss rate, is a memory-latency-bound
signature.** The workload is pointer-chasing through BSP nodes and a 277-edict array, not
arithmetic. Branch misprediction at 2.07 % is unremarkable for branchy game code and is not
the limiter.

This is consistent with §2: `SV_RecursiveHullCheck` walking BSP hull nodes is exactly the
access pattern that produces this profile.

**But keep the absolute scale in view.** The server uses 2–4 % of one core. Eliminating *every*
cache miss would recover single-digit percentages of that. This tells you *what the code is
doing*, not that there is a performance problem to solve.

## 4. An incidental find: the 32-bit build pays a PIC tax

`__x86.get_pc_thunk.ax` at **1.62 %** is pure position-independent-code overhead — the i386
trick for obtaining the program counter, since x86-32 has no PC-relative addressing. **On
x86-64 this construct does not exist at all**, because RIP-relative addressing is native.

That is a concrete mechanism behind the otherwise-unexplained observation in
`16-x86-64-running.md` §5, where the 64-bit build showed lower frame execution time (14.6 µs
vs 19.0 µs p50) — a result I explicitly declined to claim at the time. It is still not a
controlled comparison, but there is now a named reason to expect the 64-bit build to be
genuinely faster, not just differently measured.

## 5. Frame timing during live play

```
SV_ReadPackets     p50=0.5  p95=11.6  p99=23.8   share 0.17%
SV_Physics         p50=4.3  p95=47.5  p99=92.8   share 1.12%
SV_SendClientMsgs  p50=0.3  p95=36.8  p99=49.2   share 0.41%
  pfnStartFrame    p50=0.7  p95=43.2  p99=88.9   share 0.75%
admitted=1,395,461  rejected=0  overruns=20  skipped=2420
SV_Physics edicts: 276.9 visited/frame, 251.5 simulated
```

Over 1.4 million admitted frames, **20 overruns**. `SV_SendClientMessages` is more visible than
in bot-only runs (0.41 % vs 0.10 %) — expected, since a real client at `cl_updaterate 102`
demands far more snapshots than a bot.

## 6. What this does not establish

- **One real client, not ten.** The lag-comp scaling estimate in §2 is extrapolation, not
  measurement.
- **`perf` cannot see kernel symbols** here (`kptr_restrict`), so the ~6 % of samples in
  kernel space are unattributed.
- Percentages are of a process using 2–4 % of one core. Nothing here is a bottleneck in
  absolute terms; it is a map of where the cycles go.
- The build is `-g0`, so symbolisation relies on the dynamic symbol table. Inlined functions
  are attributed to their callers.
- Still no gameplay differential testing against `baseline-x86-32`.
