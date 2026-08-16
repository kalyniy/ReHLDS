# Baseline Environment and Benchmark Protocol

**Recorded:** 2026-08-15
**Baseline commit:** `0124d56c3d888d922eb045775f71c6682ad1226f` (tag `baseline-x86-32`, branch `master`)
**Working branch:** `audit/m0-baseline`
**Remote (writable):** `origin` → `https://github.com/kalyniy/ReHLDS.git` — verified, tree clean at freeze.
**Companion reference (read-only):** `/home/dan/projects/ReGameDLL_CS` @ `679973265e1ac99a43193119e0da212ee568f5f9`
(upstream `rehlds/ReGameDLL_CS`; an owner fork must be created before any ReGameDLL change is committed).

---

## 1. Host

| Property | Value |
|---|---|
| CPU | 12th Gen Intel Core i9-12900K (hybrid) |
| Logical CPUs | 24 |
| **P-cores** | **cpu0–15** — 8 physical, SMT pairs (0,1) (2,3) … (14,15), max 5100 MHz |
| **Favoured core** | **cpu8/cpu9** — max 5200 MHz (Turbo Boost Max 3.0 target) |
| **E-cores** | **cpu16–23** — 8 physical, no SMT, max 3900 MHz |
| Cache | L1d 640 KiB (16×), L2 14 MiB (10×), L3 30 MiB shared |
| NUMA | single node |
| OS | Ubuntu 24.04.4 LTS (noble) |
| Kernel | 6.17.0-22-generic, `CONFIG_HZ=1000`, `CONFIG_PREEMPT_DYNAMIC=y`, default `PREEMPT_VOLUNTARY` |
| GCC | 13.3.0 |
| Clang | 18.1.3 |
| CMake | 4.2.1 |

## 2. Benchmark hazards identified on this host

These must be controlled before any number is recorded, or the baseline is worthless.

### 2.1 CPU governor is `powersave` — **must be changed**

```
/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor = powersave
```
Frequency scaling on a latency benchmark produces bimodal results that look like
scheduler jitter. Set `performance` for all measured runs and record the setting in
every report.

### 2.2 Hybrid topology — **the server must be pinned to a P-core**

An unpinned server will be migrated between P-cores and E-cores by the scheduler. E-cores
run at 3900 MHz vs 5100 MHz and have different cache behaviour, so an unpinned run mixes
two different machines into one histogram. All measured runs must use `taskset` to a
single P-core, and the report must state which. Recommended: pin to **cpu2** (a
non-favoured P-core, leaving cpu8/9 free) with its SMT sibling **cpu3 left idle**, so
hyperthread contention does not contaminate the measurement. Record whether the sibling
was isolated.

### 2.3 Default timer slack is 50 µs — **directly material to this project**

```
/proc/self/timerslack_ns = 50000
```
The kernel is permitted to delay a timer expiry for a non-realtime task by up to 50 µs
to batch wakeups. Against the target periods:

| Target | Period | Slack as % of period |
|---|---|---|
| 1000 Hz | 1000 µs | up to **5 %** |
| 2000 Hz | 500 µs | up to **10 %** |

This is jitter injected by the kernel *before* any ReHLDS code runs, and it applies to
`nanosleep`, `select`, `poll` and friends. Two levers exist and both must be benchmarked
in Phase 2:
- `prctl(PR_SET_TIMERSLACK, 1)` on the server thread — reduces slack to 1 ns for that task.
- `SCHED_FIFO`/`SCHED_RR` — realtime tasks get zero slack, but require privilege and risk
  starving the box; treat as an opt-in mode, not a default.

Note this partly explains Finding 2.1 in `01-frame-scheduler.md` and must be measured
*separately* from the scheduler redesign, or the two effects will be confounded.

### 2.4 `PREEMPT_VOLUNTARY` is the active preemption model

`CONFIG_PREEMPT_DYNAMIC=y` means this is switchable at boot (`preempt=full`) or at
runtime via `/sys/kernel/debug/sched/preempt` (root). `preempt=full` is a candidate tail-latency
lever. Benchmark it as an explicit variable; do **not** change it silently between runs.

### 2.5 Other controls to record per run

- Turbo/thermal state and observed effective MHz during the run.
- Background load (the benchmark host must be otherwise idle).
- Context switches and CPU migrations for the server thread (`/proc/<pid>/status`,
  `perf stat -e context-switches,cpu-migrations`).
- NIC interrupt affinity — **do not tune yet** (per brief §20); record only.

## 3. Build recipe (from `.github/workflows/build.yml`, to be verified locally)

Prerequisite packages (matching CI's Debian 11 container, adapted to Ubuntu 24.04):

```bash
sudo apt-get install -y gcc-multilib g++-multilib libc6-dev-i386
```

Unit-test build and run:
```bash
rm -rf build && cmake -DCMAKE_BUILD_TYPE=Unittests -B build && cmake --build build -j24
LD_LIBRARY_PATH="rehlds/lib/linux32:$LD_LIBRARY_PATH" ./build/rehlds/engine_i486
```
CI treats exit code 0 **and 3** as success — 3 is the cppunitlite "some tests failed but
run completed" code. Record which.

Release build:
```bash
rm -rf build && cmake -B build && cmake --build build -j24
```
Produces `build/rehlds/engine_i486.so`, `build/rehlds/dedicated/hlds_linux`,
`build/rehlds/filesystem/FileSystem_Stdio/filesystem_stdio.so`, and the HLTV set.

Note: CI builds with `-j8`; local `-j24` is fine but the *measured* builds must be
recorded with their exact flags. Build parallelism does not affect the binary.

## 4. Frozen reference

`git tag baseline-x86-32` points at `0124d56c`. This tag must never move. All differential
testing compares candidate builds against binaries built from this tag with GCC 13.3.0
at `-O3`, and the resulting reference binaries should be archived with their sha256 sums
once the toolchain is available.
