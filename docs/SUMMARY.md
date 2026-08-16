# Project Summary — Findings, Fixes, Gotchas

A synthesis of `docs/audit/00`–`24`. Those are chronological research logs; this is the
consolidated result. Where a claim was later refuted, the refutation is what appears here.

---

## 1. Engine changes (the ones that are actually ours)

| change | what it fixes | evidence |
|---|---|---|
| **`-pingboost 4`** absolute-deadline scheduler | `sys_ticrate` above ~1000 was **silently inoperative**; the loop's fixed 1 ms sleep, not the tick rate, was the limiter | 06, 07, 08 |
| **catch-up-frame fix** in that scheduler | the overrun path ran frames with ~0 elapsed time — full frame cost, zero simulation, and `host_frametime` → 0 | 08 §4b |
| **`-rtprio`** (SCHED_FIFO opt-in) | the p99 residue the scheduler could not reach | 21 |
| **`Host_UpdateStats` stack overflow** | `%lu`/`%ld` into `int32`: **every 64-bit server corrupted its own stack once per second, by default** | 17 |
| **`EDICT_FROM_AREA` open-coded wrongly** | `&l[-1]` is correct on i386 *only by coincidence*; off by 8 bytes on LP64 | 16 §2.3 |
| **`string_t` signedness** | offsets from `pr_strings` are legitimately negative; unsigned turned −40 MB into +4.25 GB | 16 §2.2 |
| **`cpuid_ex` gating** | called from a library that was not linked — links fine, fails at `dlopen` | 16 §2.1 |
| **`NOXREFCHECK`** | hardcoded i386 stack asm that *assembles* on x86-64 and reads garbage — the diagnostic was silently lying | 20 §3 |
| **`sys_ticrate` warning** | tells operators when their setting does nothing | 06 §6 |
| **portability**: JIT/SSE selectable, x86-64 and AArch64 builds, Clang tree-wide | ARM64 and x86-64 servers run CS | 11, 15, 16, 20 |
| **`vec2xmm`** load helper | `_mm_loadu_ps` read **16 bytes from every 12-byte `vec3_t`** in 11 places; ASan halts on the first map load. Also removes a store-forwarding stall (IPC 0.57 → 2.98 in isolation) | 23 |
| **`SV_LerpFrame`** | our own pose rewind blended studio frames linearly, but `pev->frame` is cyclic on `[0,256)`; a bracket across the wrap posed the victim mid-animation | 23 |
| **`jitasm` null `memset`** | `memset(NULL, 0xCC, 0)` on **every server start**; UB that lets the compiler delete later null checks | 23 |
| **`crc32c` SSE loop bound** | byte index compared against a word count — the fast path did **a quarter of its intended work**. Output was always correct, so no test could see it | 23 |

Instrumentation added: frame interval / deadline lateness / overruns, per-subsystem timers,
lag-comp outcome counters, studio-cache hit rate, and rewound-pose divergence.

## 2. The headline performance result

`sys_ticrate 1000`, 10 bots, one pinned P-core:

| | stock | `-pingboost 4` | `+ -rtprio 50` + C-state cap |
|---|---|---|---|
| achieved | 928.8 Hz | 999.6 | **999.9** |
| p50 interval | 1071 µs | 999.7 | **1000.0** |
| p99 | 1316 µs | 1271 | **1005.2** |
| p99.9 | 1960 µs | 2446 | **1008.5** |
| deadline lateness p99.9 | 1646 µs | 1448 | **11.8 µs** |
| CPU | 3.4 % | 3.8 % | **1.5 %** |

At 2000 Hz the same tuning gives p99 504 µs against a 500 µs target and **lowers** CPU from
4.1 % to 1.9 %.

**Both scheduling levers are required and neither is sufficient.** C-state capping alone does
nothing; `SCHED_FIFO` alone does most of it; together the residue halves again. They fix
different causes — *when* the kernel runs the thread, versus *how fast the core wakes up*.

## 3. The defect that is measured but NOT fixed

**Lag compensation restores `origin` and nothing else.** `SV_SetupMove` reconstructs the
victim's historical position; their `angles`, `sequence`, `frame`, `blending`, `controller`
and duck hull are all read **live** at trace time, as is ReGameDLL's `m_flGaityaw` — which
supplies the **root bone matrix yaw**. The result is the right position with the wrong pose.

Measured over 13,208 rewinds, LAN client, ~37 ms rewind window:

| yaw divergence | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| | 1.80° | **19.60°** | **44.47°** | **90.09°** | 125.00° |

Sequence mismatch 3.93 %, gaitsequence mismatch 7.26 %. Internet pings widen the window and
scale this roughly linearly. **This is the largest remaining competitive-quality item and no
cvar addresses it** (`docs/audit/13`, `docs/competitive-server-config.md` §last).

The historical data is already stored and unused: `entity_state_t` in the frame history
carries `angles`, `sequence`, `frame`, `animtime`, `framerate`, `controller[4]`, `blending[4]`
and `gaitsequence`. `SV_SetupMove` reads only `origin`, `health` and `effects`.

**Caveat that shapes the fix:** `m_flGaityaw` is neither networked nor snapshotted and lives
in ReGameDLL. A complete fix spans both repos and needs new server-side per-frame history.
Rewinding only the engine-side fields while gait yaw stays live may produce a *differently*
wrong pose rather than a correct one — that needs an A/B, not intuition.

## 4. Things that were investigated and ruled out

Recorded because they are worth not re-doing.

- **Engine physics optimisation.** `SV_Physics` is 69 % of frame execution, but **97–99 % of
  its tail is `pfnStartFrame`** — GameDLL code. Optimising the engine loop cannot improve
  p99.9. (18)
- **Compiler tuning.** GCC vs Clang vs ThinLTO is unresolvable on this workload: engine code
  is ~2 % of wall time, so even a 30 % engine win moves total CPU ~0.6 points against a ~1.5
  point spread. Clang binaries are 5.9 % smaller; that is the only real difference. (19)
- **Spin windows.** `sv_rehlds_sched_spin_us` bought a better p95 for 6.6 % CPU, but **cannot
  move p99 at any window size** — which is precisely what proved the residue was a kernel
  problem. Obsoleted by C-state capping, which fixes the same cause at no CPU cost. (08 §4c, 21)
- **`EF_NOINTERP` excluding jumping/reloading victims.** Predicted from source to possibly
  dominate; measured at **0.04 %** of victim evaluations. The mechanism is real, the magnitude
  is not. (12)
- **Studio-cache hit rate.** 4.9–16.2 %, and low *by construction* — `origin` and `frame` are
  in the key, so it is a same-tick memoiser for multi-pellet weapons, not a general cache.
  Raising the ratio is not a useful goal. (09 §2)
- **Blockers that never materialised**: `edict_t`/`entvars_t` layout matched exactly on 64-bit
  (864 / 720 bytes both sides); `.hpk` mismatch is detected and recovered rather than fatal.

## 5. Gotchas — things that will waste hours

**Build / toolchain**
- CMake 4 rejects `cmake_minimum_required(3.1)`: pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- The bundled `lib/linux32/librt.so` imports GLIBC_PRIVATE symbols **removed in glibc 2.34**,
  so the engine cannot link on Ubuntu 22.04+ without `-DUSE_VENDORED_COMPAT_LIBS=OFF`.
- HLDS bundles an ancient `libstdc++` and the engine has `RUNPATH $ORIGIN`, so it wins over
  the system one. Build with `-DUSE_STATIC_LIBSTDC=ON` to run against a stock install.
- Installing the ARM64 cross-toolchain makes apt remove `gcc-multilib`, which takes
  `linux-libc-dev:i386` with it and silently breaks all 32-bit builds. Reinstall **that
  package**, not the metapackage — the metapackage removes the cross-toolchain again.
- The HLTV sub-projects pass `-flto` at compile but not at link. GCC's linker plugin covers
  for it; Clang emits pure bitcode and fails. Fixed here.

**Running**
- `steamclient.so` is not in the app-90 install; symlink it from steamcmd or the server dies
  with `FATAL ERROR: Unable to initialize Steam` even with `-insecure`.
- zBots: the profile archive ships **in the ReGameDLL repo**; `bot_enable 1` goes in
  `cstrike/game_init.cfg`; and **`bot_join_after_player 0`** is required or bots silently
  never join, with no diagnostic beyond `players : 0 active`. `.nav` auto-generates on first
  bot spawn (~60 s).

**Measuring**
- `perf(1)` needs `perf_event_paranoid` lowered. In-engine subsystem timers were used instead.
- **Bot workloads vary up to 54 % run to run.** A single run is not evidence. One single-run
  comparison produced a confident, entirely fabricated finding about hitbox asymmetry that had
  to be retracted (10). `gameplay_ab.sh` runs each condition twice for this reason.
- Bots **cannot** exercise lag compensation at all — `SV_RunCmd` gates `SV_SetupMove` on
  `!host_client->fakeclient`. Any hitreg work needs a real client.
- The frameperf ring holds 8192 samples — at 1000 Hz that is the last ~8 s, not the whole run.
  Whole-window CPU is the more stable comparison metric.

## 6. Method notes worth keeping

Several source-derived predictions were **wrong**, and only measurement caught them:

- `EF_NOINTERP` was predicted to dominate; it is 0.04 %.
- `string_t` was ranked the hardest 64-bit blocker, with a proposed arena or an ABI break.
  Measurement showed offsets of only −40 MB and +3.4 MB — the values were never large, only
  mis-signed. The fix is one typedef.
- Three claims from an automated portability sweep failed verification (inert static asserts,
  a "missing" equivalence test that already existed, an unreachable buffer overrun).

Source reading is reliable for *what the code does* and unreliable for *how much it matters*.

## 7. Outstanding

1. **Hit registration** — §3. The largest remaining competitive item; needs a real client and
   spans both repos. The remaining piece is **gait yaw**: ReGameDLL overwrites the model's root
   yaw with the live `m_flGaityaw` (`animation.cpp:1183`), so the engine-side angle rewind never
   reaches hitbox orientation. `docs/audit/24` designs the fix and establishes it costs nothing
   on the wire — lag compensation reads the server's own snapshot history, not network data, so
   any `entity_state_t` field the GameDLL fills is rewindable whether or not `delta.lst`
   transmits it. Deliberately unimplemented: it cannot be validated without a real client, and
   shipping unexercised hit-registration changes is what produced the frozen-corpse regression.
2. **The lag-comp path is still unsanitized.** ASan now reports 0 errors on the production
   32-bit build, but bots never enter `SV_SetupMove` (`SV_RunCmd` gates it on
   `!fakeclient`), so the rewind and pose-restore code has never been under a sanitizer. This is
   the highest-value place to point ASan next, and it needs one human connected.
3. **Lag-comp cost with 10 real players** — the one unmeasured hotspot. The 1.7 µs
   `SV_ReadPackets` figure excludes it entirely, because bots never trigger it.
4. **Gameplay differential testing** — nothing here has been compared against the frozen
   `baseline-x86-32` reference. Note `mathlib_e.h:35-40` exists to compensate for i386 x87
   excess precision, so cross-architecture *bit-identical* output is not achievable;
   tolerances must be defined per quantity.
5. **A deterministic workload** (demo replay or scripted Protocol 48 clients) — gates 3 and 4
   simultaneously, and would also make compiler comparison resolvable.
6. **Real ARM64 hardware** — everything AArch64 so far is emulated.
