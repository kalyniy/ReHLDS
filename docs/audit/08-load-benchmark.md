# Measurement 08 — Scheduler Under Competitive Load (10 zBots)

**Date:** 2026-08-15
**Engine:** `audit/m0-baseline` @ `cf66ce9` + instrumentation
**GameDLL:** ReGameDLL_CS 5.30.0.823-dev, built from the **owner fork**
`kalyniy/ReGameDLL_CS` @ `6799732` (tag `baseline-regamedll`, branch `audit/bench-support`)
**Conditions:** i9-12900K, governor `performance`, pinned `taskset -c 2` (P-core, SMT
sibling idle), `de_dust2`, **10 zBots**, `bot_difficulty 2`, 40 s warmup, 25 s window
**Harness:** `docs/audit/tools/bench.sh`

This supersedes the empty-server figures in `07-deadline-scheduler.md` §3 for any
conclusion about tail latency. Load changes the answer in a way the empty server hid.

---

## 1. Benchmark environment is now reproducible

zBot setup, per the ReGameDLL README:

1. Extract `regamedll/extra/zBot/bot_profiles.zip` (ships **in the repo** — no third-party
   download) into the game root; it provides `cstrike/BotProfile.db`, `BotChatter.db` and
   487 radio `.wav` files. Contents were listed and verified before extraction: no
   executables.
2. `bot_enable 1` in `cstrike/game_init.cfg` — this file does not exist by default.
3. `bot_join_after_player 0`, otherwise bots wait for a human and the server silently stays
   empty. This was the cause of the first failed attempt, which produced no diagnostic at
   all beyond `players : 0 active`.
4. A `.nav` mesh is required. `maps/de_dust2.nav` does **not** ship with the steamcmd
   install; the first bot spawn on a navless map triggers `StartLearnProcess()`
   (`regamedll/dlls/bot/cs_bot_init.cpp:348-352`) and generates it automatically. Takes
   ~60 s and produces a 422 KB file that persists.

Frame execution p50 rises from **8.1 µs** (empty) to **18.0 µs** (10 bots), so this is real
work, though still only ~2 % of a 1000 µs budget.

## 2. Results at `sys_ticrate 1000`

| config | achieved | p50 | p95 | p99 | p99.9 | lateness p50 | exec p50 | overruns | CPU | ctxsw/s |
|---|---|---|---|---|---|---|---|---|---|---|
| default pingboost | 912.1 | 1081.3 | 1172.8 | 1388.5 | 2073.9 | 537.7 | 18.0 | 784 | 3.4 % | 1 159 |
| **`-pingboost 4`** | **999.6** | **999.7** | **1025.2** | **1303.0** | 2436.8 | **12.3** | 18.7 | **31** | 3.8 % | 1 246 |
| **`-pingboost 4`, spin 100 µs** | **999.8** | **1000.0** | **1001.4** | **1289.1** | 2175.6 | **0.5** | **4.3** | **24** | 11.1 % | 1 258 |

## 3. Results at `sys_ticrate 2000`

| config | achieved | p50 | p95 | p99 | p99.9 | lateness p50 | exec p50 | overruns | CPU | ctxsw/s |
|---|---|---|---|---|---|---|---|---|---|---|
| `-pingboost 3` | 1872.6 | 531.8 | 550.6 | 786.9 | **1227.4** | 275.3 | 3.0 | 542 | 5.7 % | **18 010** |
| `-pingboost 4` | 1992.9 | 499.9 | 519.4 | 810.8 | 2026.0 | 11.1 | 16.4 | 83 | 5.5 % | 2 246 |
| `-pingboost 4`, spin 25 µs | 1995.6 | 500.0 | 508.4 | 722.0 | 1735.7 | 2.4 | 16.5 | 74 | 9.1 % | 2 243 |
| `-pingboost 4`, spin 50 µs | 1996.3 | 500.0 | 502.9 | 780.5 | 1636.5 | 0.6 | 3.0 | 82 | 11.4 % | 2 253 |
| **`-pingboost 4`, spin 100 µs** | **1999.3** | **500.0** | **502.0** | **620.0** | **1230.4** | **0.5** | **2.9** | **55** | 21.2 % | 2 281 |

## 4. The honest weakness, and its cause

**Without a spin window, `-pingboost 4` has a worse far tail than `-pingboost 3` under
load** — p99.9 of 2026 µs against 1227 µs at 2000 Hz, and 2437 vs 2074 at 1000 Hz. The
empty-server runs in `07-deadline-scheduler.md` did not show this. Reporting only those
would have overstated the result.

The mechanism is visible in a column that is not about scheduling at all. **Frame execution
p50**:

| | exec p50 |
|---|---|
| `-pingboost 3` (spins constantly) | 3.0 µs |
| `-pingboost 4`, no spin (sleeps ~480 µs) | 16.4 µs |
| `-pingboost 4`, spin 100 µs | 2.9 µs |

The *same engine code doing the same work* runs **5.6× slower** after a real sleep. That is
not scheduling jitter — it is the core going cold: C-state exit latency, frequency ramp, and
cold caches. `-pingboost 3`'s spin was accidentally buying warm-core execution, and paying
18 010 context switches per second for it.

A guard window recovers both. At 100 µs the spin is long enough to hold the core in a shallow
idle state, and execution p50 returns to 2.9 µs while p99.9 matches mode 3 (1230 vs 1227 µs).

**With spin 100 µs, `-pingboost 4` beats `-pingboost 3` on every latency metric
simultaneously:**

| metric | `-pingboost 3` | `-pingboost 4` + spin 100 | change |
|---|---|---|---|
| achieved Hz | 1872.6 | 1999.3 | +6.8 % |
| p50 interval | 531.8 µs | 500.0 µs | exact |
| p95 | 550.6 µs | 502.0 µs | −8.8 % |
| p99 | 786.9 µs | 620.0 µs | −21 % |
| p99.9 | 1227.4 µs | 1230.4 µs | tie |
| deadline lateness p50 | 275.3 µs | 0.5 µs | **550×** |
| overruns / 8192 | 542 | 55 | −90 % |
| context switches/s | 18 010 | 2 281 | **7.9× fewer** |
| CPU | 5.7 % | 21.2 % | **+15.5 pp** |

The cost is CPU: 21.2 % of one core versus 5.7 %. On a 24-thread host dedicating one pinned
core to the server this is an acceptable trade for a competitive match server and a poor one
for a busy public server. **Hence `sv_rehlds_sched_spin_us` defaults to 0** and is an
operator decision, not a default.

25 µs is *not* enough — execution p50 stays at 16.5 µs, i.e. the core still goes cold. The
useful threshold on this host is between 25 and 50 µs.

## 5. Cheaper alternative worth testing before accepting the spin cost

The spin is a blunt instrument for what is really a C-state problem. Two levers should be
benchmarked before recommending a 21 % CPU spin:

- Writing `0` to `/dev/cpu_dma_latency` and holding the fd open caps CPU idle exit latency
  system-wide without burning a core in userspace.
- `idle=poll` or per-core `cpuidle` state disabling via
  `/sys/devices/system/cpu/cpu2/cpuidle/state*/disable`.

If either recovers the warm-core execution time, the spin window becomes unnecessary and the
`-pingboost 4` result improves to "strictly better than mode 3 on every axis including CPU."
Both require root, so they are logged rather than tested here.

## 6. Still not established

- **No gameplay correctness validation.** Bots run, rounds proceed, no crashes across all
  seven runs — but nothing has differential-tested movement, traces, spread or round logic
  against `baseline-x86-32`. The `Host_FilterTime` gate bypass changes *when* frames run,
  and that is exactly the kind of change the brief requires differential evidence for.
  **This is the blocking item before `-pingboost 4` is production advice.**
- The 8192-sample ring covers only the last ~4–9 s of each 25 s window; p99.9 rests on
  ~8–50 samples and `max` on one. Single run per configuration. Repeat-run variance is
  unmeasured, and some of the p99/p99.9 ordering between adjacent spin settings (e.g. spin 50
  showing a worse p99 than spin 25) is likely noise rather than signal.
- Bots are not humans: no network traffic, no lag compensation exercised, no client
  prediction. Bot AI also adds server CPU that a real 10-human server would not have.
- Only `de_dust2`, only `bot_difficulty 2`.
