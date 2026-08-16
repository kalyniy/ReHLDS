# Agent Handoff — ReHLDS Modernization on Apple Silicon

You are picking up an in-progress ReHLDS modernization project. Read this file completely
before running anything. It tells you the rules, the current state, what is verified, what is
not, and how to set up on an Apple Silicon Mac.

---

## 0. STOP — non-negotiable rules

**Security / provenance.** ReHLDS upstream issue **#1190** and PR **#1191** (July 2026) are
**quarantined** by the project owner as a suspected malware attempt. You must not merge,
cherry-pick, download, build, execute, or reverse-engineer anything from that proposal, its
author's repositories or branches, or any binary/archive linked from it or distributed via
DM/social media. Reading the public discussion as historical context is the only permitted
use. Any hitbox defect must be reproduced independently from the current source and fixed
with original code.

**Licensing.** `Garey27/hitbox_fixer` may be *source-reviewed* as a conceptual reference
only. It is GPL-3.0 and this fork is MIT — **do not copy any of its code**, and do not make
the project depend on it.

**Repositories.** All work belongs in the owner's forks:
- Engine — `https://github.com/kalyniy/ReHLDS`
- GameDLL — `https://github.com/kalyniy/ReGameDLL_CS`

Treat `rehlds/*` upstream as read-only reference. Never push there, never open upstream PRs.

**Working method the owner expects.** Measure before optimising; attach evidence to claims;
report distributions (p50/p95/p99/p99.9), never average FPS alone; small reviewable commits;
verify agent/tool claims before acting on them. Work autonomously — decide and proceed rather
than asking which task to do next. Reserve questions for credentials, sudo, destructive or
outward-facing actions.

---

## 0b. The owner's goal for the Mac

**Run the HLDS *server* natively on Apple Silicon (MacBook Pro / Mac mini), and play from a
separate Windows PC.** No client on the Mac — that is explicitly not wanted, and is
impossible anyway (§4.1).

Bots are optional: if zBots run on the Mac they are welcome as load, otherwise an empty but
running server is acceptable. **Bots do work** — ReGameDLL's zBots have been verified running
10-strong on AArch64, so they will run here too (§8b).

Read §4b before promising "native", because the word has two meanings on this hardware and
only one of them is available today.

## 1. What this project is

Make an owner-controlled ReHLDS fork measurably better for competitive CS 1.6: deterministic
hit registration, stable 1000/2000 Hz scheduling judged on tail latency, and portability to
x86-64 and ARM64 — all without changing what stock 32-bit CS 1.6 clients see on the wire
(Protocol 48).

**Full engineering detail lives in `docs/audit/00`–`20`.** Read `docs/audit/` in order; each
file records what was measured, what was concluded, and explicitly what was *not* established.
Several documents contain retractions where measurement refuted an earlier claim — those are
deliberate and worth reading.

## 2. Current state

| target | status |
|---|---|
| **x86-32** | production target. Builds, 33/33 unit tests, runs. **See §6 warning.** |
| **x86-64** | runs CS with 10 bots at ~998 Hz, 32/32 tests |
| **AArch64 Linux** | runs CS with 10 bots under qemu-user, 31/31 tests |
| **arm64 macOS (Darwin)** | **does not exist.** Not started. See §4. |

Headline results so far:

- **`-pingboost 4`** — a new absolute-deadline scheduler. Proved `sys_ticrate 2000` was
  silently inoperative under the old modes (927.6 Hz vs 928.8 at `sys_ticrate 1000`). Now
  999.6 / 1995 Hz with ~8× fewer context switches.
- **Lag compensation restores `origin` and nothing else** — the victim's pose is live while
  their position is historical. Measured yaw divergence: 19.6° at p90, 90° at p99.9, on a
  LAN client with only a 37 ms rewind window.
- **A real bug ASan found**: `Host_UpdateStats` parsed `/proc/<pid>/stat` with `%lu`/`%ld`
  into `int32`, so every 64-bit server corrupted its own stack once per second, by default.
- **Ruled out**: engine physics optimisation (97–99% of `SV_Physics`'s tail is GameDLL code)
  and compiler tuning (effect is ~10× below the benchmark's noise floor).

## 3. Branches

| repo | branch | contents |
|---|---|---|
| `kalyniy/ReHLDS` | `audit/m0-baseline` | all engine work, 24 commits, tagged `baseline-x86-32` at the frozen starting commit |
| `kalyniy/ReGameDLL_CS` | `arm64/port` | one source fix (`DebuggerBreak` `int3` → `brk #0`) |

Both branch from unmodified upstream. `git log --oneline` on each gives a readable history;
commit messages are long and carry the reasoning.

## 4. Apple Silicon: read this before planning anything

**M5 Max is arm64. Three constraints follow, and they are hard.**

1. **There is no i386 on Apple Silicon at all.** Rosetta 2 translates **x86-64 only** — it has
   never supported 32-bit x86 — and macOS dropped 32-bit support entirely in Catalina. The
   current production build of ReHLDS is 32-bit x86. **It cannot be built or run on this Mac,
   natively or emulated.** If you need to touch the 32-bit build, use an x86-64 Linux host.
2. **The existing ARM64 work is `aarch64-linux-gnu`, not `arm64-apple-darwin`.** ELF vs
   Mach-O, glibc vs libSystem, GNU ld vs ld64. Sharing an instruction set is the easy half; a
   native macOS port has *not* been started and is a separate project (the brief sequences it
   after ARM64 Linux).
3. **What Apple Silicon is genuinely good for right now:** running the AArch64 **Linux** build
   **natively, at full speed**, in an arm64 Linux container. Everything measured so far on
   ARM64 was under qemu emulation, which says nothing about performance and papers over
   memory-ordering issues. **A native arm64 Linux container on an M5 Max is exactly the
   missing "real ARM64 hardware" test** — that is the single most valuable thing this machine
   can contribute.

### Recommended setup

Use **OrbStack** (lighter and faster than Docker Desktop on Apple Silicon) or Docker Desktop:

```bash
brew install orbstack        # or: brew install --cask docker
```

Then an arm64 Ubuntu container — note **no** `--platform` flag, so it runs natively:

```bash
docker run -it --name rehls-arm64 -v ~/rehlds-work:/work ubuntu:24.04 bash
```

Inside the container:

```bash
apt-get update && apt-get install -y \
    build-essential cmake git python3 gdb \
    rsync curl ca-certificates
```

That is all the AArch64 build needs — it compiles natively there, no cross-toolchain and no
qemu.

## 4b. "Natively on Mac" — two meanings, be precise about which

The owner's goal (§0b) is a native Mac server. There are two readings and they are very
different in cost:

**(a) Native-speed ARM64 Linux in a container — available today.**
On Apple Silicon, Docker/OrbStack run a lightweight Linux VM via Apple's
Virtualization.framework. The CPU executes AArch64 instructions **directly** — no
translation, no qemu, full core performance. Our `engine_arm64.so` runs there as-is. This is
what §4's setup gives you and it works right now.

**The caveat that matters for THIS project:** it is still a VM. This project exists to chase
microsecond-scale tail latency — `-pingboost 4` holds p50 within 0.1 µs of target and the open
question is p99. A virtualisation layer adds timer and scheduling jitter on top of the guest
kernel's own. **Whether that is acceptable is unmeasured and is the single most important
experiment this machine can run.** Do not assume it is fine, and do not assume it is fatal —
measure it with `docs/audit/tools/bench.sh` and compare against the x86-64 Linux numbers in
`docs/audit/08`.

**(b) A true `arm64-apple-darwin` build — does not exist, and is a real project.**
Not started. Sharing an instruction set is the easy part. Known blockers, none of them
cosmetic:

- **`clock_nanosleep` does not exist on macOS.** The `-pingboost 4` deadline scheduler is
  built on `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`. Darwin needs `mach_wait_until()`
  plus `mach_absolute_time()`. The project's headline feature needs a second implementation.
- **`prctl(PR_SET_TIMERSLACK)` does not exist**; Darwin uses thread QoS classes and
  `thread_policy_set`. That was worth ~45 Hz on Linux.
- **`RTLD_DEEPBIND` does not exist on macOS.** The engine uses it to load the GameDLL with
  isolated symbol resolution; Darwin's two-level namespace behaves differently and needs
  thought, not a flag swap.
- **`/proc` does not exist.** `Host_UpdateStats` parses `/proc/<pid>/stat`; Darwin needs
  `proc_pidinfo` / `task_info`.
- **`ld64` has no `--wrap`.** `filesystem_stdio`'s `pathmatch.cpp` relies on the GNU linker's
  symbol wrapping (`__real_*` / `__wrap_*`). There is no direct equivalent; it needs
  restructuring or `DYLD_INTERPOSE`.
- Mach-O rather than ELF: `.dylib` naming, no version scripts (use `-exported_symbols_list`),
  different PIC/PIE rules.
- Steam: a macOS `libsteam_api.dylib` does ship with both x64 and arm64 slices, but the
  interface-version mismatch in §8 applies regardless.

Note the game install already contains `cstrike/dlls/cs.dylib`, so Valve did ship a macOS
GameDLL historically — ReGameDLL may port more easily than the engine.

**Recommendation.** Do (a) first and measure the VM's tail-latency cost. That answer decides
whether (b) is worth doing at all — and if (a) turns out fine, (b) may never be needed.
A third option, bare-metal Linux via Asahi, is unlikely to be viable: Asahi targets M1/M2 with
later chips in progress, so an M5 Max is almost certainly unsupported.

### If you also need x86 targets on this Mac

`--platform linux/amd64` gives you an emulated x86-64 container (Rosetta-backed under
OrbStack, qemu under Docker). It works but is slow, and **i386 still will not work** for §4.1.
For any 32-bit work, use a real x86-64 Linux machine.

## 5. Game content — already on the Mac

**`steamcmd` is x86-only and will not run on arm64 — but you do not need it.** It is only a
downloader; the game content is architecture-independent data (296 MB `cstrike/` + 433 MB
`valve/`: maps, models, sounds, WADs). The only arch-specific files are the game DLL, which we
build, and `cl_dlls/`, which is client-side and irrelevant to a server.

**The owner already installed Steam and CS 1.6 on this Mac.** The game would not launch —
correctly, see §4.1 — but Steam still downloaded the content. It is at roughly:

```
~/Library/Application Support/Steam/steamapps/common/Half-Life/
```

Mount that into the container read-only and copy out `cstrike/` and `valve/`, or point the
server at a writable copy:

```bash
cp -R ~/Library/Application\ Support/Steam/steamapps/common/Half-Life/{cstrike,valve} \
      ~/rehlds-work/hlds/
```

Fallbacks if that install is missing or incomplete: copy from the x86-64 Ubuntu box
(`~/projects/hlds`), or run `steamcmd` in an emulated `linux/amd64` container.

The zBot files were already installed there: `cstrike/BotProfile.db`, `BotChatter.db`,
`sound/radio/bot/*`, `bot_enable 1` in `cstrike/game_init.cfg`, and a generated
`cstrike/maps/de_dust2.nav`. Preserve all of them — regenerating the nav mesh takes ~60 s and
requires `bot_join_after_player 0` or bots silently never join.

## 6. WARNING — unverified 32-bit build

The last change set (ARM64 support) touched shared files: `osconfig.h`, `maintypes.h`,
`common.cpp`, `sys_shared.cpp`, `crc32c.cpp`, `sse_mathfun.*`, `pathmatch.cpp` and several
`CMakeLists.txt`. **Every change is guarded so i386 keeps its original code path**, but the
32-bit build could not be *verified*, because installing the ARM64 cross-toolchain on the
Ubuntu host made apt remove `gcc-multilib` and with it `linux-libc-dev:i386`.

**Guarded is not verified, and 32-bit is the production target.** On an x86-64 Linux host:

```bash
sudo apt install linux-libc-dev:i386   # NOT gcc-multilib - that removes the arm64 cross tools
cd ReHLDS
rm -rf build && cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUSE_VENDORED_COMPAT_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Unittests -B build && cmake --build build -j$(nproc)
LD_LIBRARY_PATH="rehlds/lib/linux32:$LD_LIBRARY_PATH" ./build/rehlds/engine_i486
# expect: There were no test failures; Tests executed: 33
```

**Do this before trusting anything else.**

## 7. Build commands

Two flags are needed on any modern host and are not defaults:

- `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` — CMake 4 rejects the tree's `cmake_minimum_required(3.1)`.
- `-DUSE_VENDORED_COMPAT_LIBS=OFF` — the bundled `lib/linux32/librt.so` imports GLIBC_PRIVATE
  symbols removed in glibc 2.34, so the engine cannot link on Ubuntu 22.04+ without this.
  Leave it ON only for legacy-distro release builds.

**AArch64, natively in an arm64 container** (the interesting case on this Mac):

```bash
# unit tests
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUSE_VENDORED_COMPAT_LIBS=OFF \
      -DCMAKE_BUILD_TYPE=Unittests -B build-arm64
cmake --build build-arm64 -j$(nproc)
./build-arm64/rehlds/engine_arm64        # expect 31 tests, exit 3 (= success)

# server
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUSE_VENDORED_COMPAT_LIBS=OFF -B build-arm64rel
cmake --build build-arm64rel -j$(nproc)
```

Note: building *on* arm64 needs no toolchain file. `cmake/toolchain-aarch64.cmake` exists for
*cross*-compiling from x86-64 and is not needed here.

ReGameDLL, same container:

```bash
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DXASH_COMPAT=ON -B build-arm64
cmake --build build-arm64 -j$(nproc)     # -> regamedll/cs_arm64.so
```

`REHLDS_ENABLE_JIT` and `REHLDS_ENABLE_SSE` auto-disable off x86 with a status message — the
delta JIT is an IA-32 code generator and SSE is x86-only. Both have portable fallbacks that
the unit tests exercise.

## 8. Assembling and running a server

Engine `.so`, `hlds_linux` and `filesystem_stdio.so` go in the game root; the GameDLL goes to
`cstrike/dlls/cs.so` (the engine reads that name from `liblist.gam`, so `cs_arm64.so` must be
copied *as* `cs.so`).

```bash
cd ~/rehlds-work/hlds
./hlds_linux -game cstrike -console -nomaster -insecure \
    +sv_lan 1 +map de_dust2 +maxplayers 12 +sys_ticrate 1000 -pingboost 4 -port 27015
```

`-insecure` is required: the 64-bit/ARM64 builds link a generated **Steam stub**
(`engine/steam_stub_64.cpp`) with no authentication, no VAC and no master server. The real
64-bit `libsteam_api.so` implements `SteamGameServer015` while these headers are pinned to
`SteamGameServer011`, so it is not a drop-in — that remains an open workstream.

## 8b. Bots on Apple Silicon, and connecting from the Windows PC

**Bots work.** ReGameDLL's zBots were verified running 10-strong on AArch64 (under emulation;
native will be faster). They are pure game code with no architecture dependency beyond what
the GameDLL already needed.

Setup, in this order — the middle step is the one that silently fails if skipped:

1. Extract `regamedll/extra/zBot/bot_profiles.zip` from the ReGameDLL repo into the game root.
   It provides `cstrike/BotProfile.db`, `BotChatter.db` and 487 radio `.wav` files. It ships
   *in the repo*, so no third-party download.
2. `bot_enable 1` in `cstrike/game_init.cfg` (create the file; it does not exist by default).
3. **`bot_join_after_player 0`** — otherwise bots wait for a human and the server sits empty
   with no diagnostic beyond `players : 0 active`.
4. `bot_quota 10`. On a navless map the first bot spawn auto-generates `maps/<map>.nav`
   (~60 s, ~422 KB, persists).

**Connecting from the Windows PC.** Publish the game port from the container — GoldSrc is UDP:

```bash
docker run -it -p 27015:27015/udp ...
```

Then from CS 1.6 on the PC: `connect <mac-lan-ip>:27015`. Use `sv_lan 1` and `-insecure` (the
Steam stub means no authentication either way, §8). If the client cannot see the server,
check the Mac's firewall and that the container published **UDP**, not TCP — publishing TCP
only is the usual mistake and produces a silent failure.

### Instrumentation

```
sv_rehlds_perf_frame 1     ->  rehlds_perf_frame_dump     # interval/lateness/overruns + subsystem timers
sv_rehlds_perf_hitreg 1    ->  rehlds_perf_hitreg_dump    # lag-comp outcomes, studio cache, pose divergence
sv_rehlds_sched_spin_us N  # optional busy guard window for -pingboost 4, default 0
```

`docs/audit/tools/bench.sh` is a reproducible 10-bot benchmark; `gameplay_ab.sh` runs each
condition **twice** because bot workloads vary up to 54% run-to-run — a single run is not
evidence, and a single-run comparison already produced one fabricated finding that had to be
retracted (`docs/audit/10`).

## 9. Highest-value next steps

1. **Verify the 32-bit build** (§6). Blocking everything.
2. **Native ARM64 performance measurement.** Everything ARM64 so far is emulated. Running
   `bench.sh` natively on the M5 Max is new information nobody has, and it also exposes
   memory-ordering behaviour qemu hides. Note `docs/audit/17` lists 18 unaligned accesses that
   are UB but tolerated by AArch64 Linux — real hardware plus an optimiser is where they could
   bite.
3. **A deterministic workload** — demo replay or scripted Protocol 48 clients. This is the
   acknowledged bottleneck: bots cannot exercise lag compensation at all (`SV_RunCmd` gates it
   on `!host_client->fakeclient`), they inflate GameDLL cost, and their variance swamps
   sub-10% effects. It gates gameplay differential testing, compiler discrimination, and the
   hitbox work simultaneously.
4. **Gameplay differential testing** before `-pingboost 4` or any 64-bit build is called
   production-ready. Note `mathlib_e.h:35-40` exists to compensate for i386 x87 excess
   precision, so cross-architecture *bit-identical* gameplay is not achievable — tolerances
   must be defined per quantity.
5. **The hitbox fix.** Deferred by owner preference. Spans both repos: `m_flGaityaw` supplies
   the root bone matrix yaw, lives in ReGameDLL, and is neither networked nor snapshotted.

## 10. Things that will waste your time if you do not know them

- `perf(1)` needs `perf_event_paranoid` lowered (root). In-engine subsystem timers exist
  instead and attribute to engine stages rather than symbols.
- ASan cannot load a library opened with `RTLD_DEEPBIND`; the code drops that flag under
  sanitizers automatically.
- The frame instrumentation ring holds 8192 samples — at 1000 Hz that is the last ~8 seconds,
  not the whole run. Whole-window CPU is the more stable metric for comparisons.
- HLTV is unported and is skipped automatically for non-32-bit-x86 builds.
- `USE_STATIC_LIBSTDC=ON` is needed to run against a stock steamcmd HLDS install, which
  bundles an ancient `libstdc++` and whose `$ORIGIN` runpath wins over the system one.
