# Milestone 16 — A 64-bit ReHLDS Server Running Counter-Strike

**Date:** 2026-08-16
**Status:** `engine_amd64.so` + 64-bit `hlds_linux` + `filesystem_stdio.so` + ReGameDLL's
`cs_amd64.so` run `de_dust2` with 10 zBots, at 999.5 Hz under `-pingboost 4`, with the frame
and hit-registration instrumentation working.

**This is a development build, not a production one.** It has no Steam authentication (see
§4), and its gameplay has not been differential-tested against the 32-bit reference.

---

## 1. Verified working

```
--- frameperf: 8192 samples over 8.20s ---
  sys_ticrate=1000  target_period=1000.0 us  achieved=999.5 Hz
  frame interval     p50=    999.8 p95=  1015.6 p99=  1104.7 p99.9=  1752.0 us
  frame execution    p50=     14.6 p95=    59.8 p99=   125.0 p99.9=   292.7 us
  SV_Physics         p50=     10.3 ... share of wall time: 1.86%
```

10 bots connected, rounds running, studio hulls constructed, player hits registering.

## 2. The four defects that stood between compiling and running

The compile stage was nearly free (`15-x86-64-recon.md`). Everything real was at runtime, and
each one surfaced only by running.

### 2.1 `cpuid_ex` — undefined symbol at `dlopen`

`sys_shared.cpp` gated `cpuid_ex` on `ASMLIB_H` alone. `precompiled.h` includes `asmlib.h`
unconditionally, so the symbol is *declared* even in builds that do not link the 32-bit-only
`libaelf32.a`. Shared objects permit undefined symbols, so it linked and then failed to load.
`strtools.h:67` already required `ASMLIB_H && HAVE_OPT_STRTOOLS`; `sys_shared.cpp` now
matches.

**Wider lesson:** dropping asmlib costs more than the string functions. The portability audit
recorded the `strtools.h` fallback as complete and missed that `cpuid_ex` comes from the same
library.

### 2.2 `string_t` truncation — B1, and much smaller than predicted

Crashed in `ED_ParseEdict` at `pr_edict.cpp:197`:

```c
className = (char *)(pr_strings + ent->v.classname);   // -> 0x8000f48a2bb2, SIGSEGV
```

Measured the actual address-space layout rather than assuming:

| | address | offset from `pr_strings` |
|---|---|---|
| `pr_strings` (= `gNullString`, engine rodata) | `0x7ffff6f19709` | 0 |
| `Ed_StrPool` hunk | `0x7ffff48a2ba0` | **−40,332,137** |
| `g_psv` (engine bss) | `0x7ffff725b7a0` | **+3,416,215** |

**Both fit a signed 32-bit int with room to spare.** The failure was purely that `string_t`
is *unsigned*: −40 MB became +4.25 GB, and reconstructing the pointer walked off the address
space.

The fix is therefore not a bounded string pool and not a widened type — it is one word:

```c
#if defined(__x86_64__) || defined(__aarch64__)
typedef int string_t;          // signed
#else
typedef unsigned int string_t; // unchanged on i386
#endif
```

plus removing the `(unsigned int)` casts in the two `STRING()` macros that would have undone
it. This is **exactly** what ReGameDLL's 64-bit build already expects —
`regamedll/dlls/qstring.h:37` declares `using qstring_t = int` under `XASH_64BIT` — so engine
and GameDLL agree without any ABI break or plugin breakage.

`04-x86-64-portability.md` ranked this the hardest blocker and proposed either a sub-4 GB
arena or widening the type with an unavoidable GameDLL ABI break. **Both were wrong**: the
measurement showed the offsets were never large, only mis-signed. A second `string_t` typedef
in `maintypes.h:61` also had to be changed in step.

### 2.3 `EDICT_FROM_AREA` open-coded wrongly — a new find

After the map loaded, `SV_AddLinksToPM_` failed `NUM_FOR_EDICT: bad pointer`, reached via
ReGameDLL's bot `StartFrame` → `PF_RunPlayerMove_I` → `SV_RunCmd`.

```c
check = (edict_t *)&l[-1];    // sv_user.cpp:535
```

Recovering an edict from its area link by subtracting **one `link_t`**. `link_t` is two
pointers: 8 bytes on i386, 16 on LP64. `offsetof(edict_t, area)` is 8 on both. So on i386
`sizeof(link_t) == offsetof(edict_t, area)` **by coincidence** and the expression is correct;
on x86-64 it lands 8 bytes before the edict.

The correct macro already existed and is used at four other traversal sites in `world.cpp`:

```c
#define STRUCT_FROM_LINK(l,t,m) ((t *)((byte *)l - offsetof(t, m)))
#define EDICT_FROM_AREA(l)      STRUCT_FROM_LINK(l,edict_t,area)
```

`sv_user.cpp` now uses it. This is a genuine latent bug that no amount of source reading had
surfaced — it took running the thing.

### 2.4 Build-system arch assumptions

`dep/bzip2` hardcoded `-m32`; the engine target hardcoded `POSITION_INDEPENDENT_CODE OFF`,
which x86-64 shared objects cannot use; the launcher and filesystem hardcoded `-m32`; and
`ENGINE_LIB` hardcoded `engine_i486.so`. All now follow `REHLDS_TARGET_BITS`, with the 64-bit
engine named `engine_amd64.so` per the ReGameDLL/Xash3D convention that `common/port.h`'s
long-dead `SO_ARCH_SUFFIX` was written for.

## 3. Blockers that did **not** materialise

- **`edict_t` / `entvars_t` layout (B2).** Measured identical on both sides:
  `sizeof(edict_t) = 864`, `sizeof(entvars_t) = 720` for engine and GameDLL alike. The audit
  ranked this the #2 blocker; because both are compiled from compatible headers with the same
  `string_t` width, it simply agrees.
- **`.hpk` format (B4)** did surface, but benignly: the 64-bit server logs
  `Mismatched data in HPAK file custom.hpk, deleting` against a file written by the 32-bit
  build, detects the inconsistency and recreates it. It does not crash. It does mean
  custom-content paks are **not** portable between architectures, which still needs an
  explicit fixed-width serializer before either can be shipped.

## 4. What is still missing before this is a real server

1. **No Steam.** `engine/steam_stub_64.cpp` (generated) satisfies the 15 Steamworks symbols
   so the build links and runs `-insecure`. There is no authentication, no VAC, no master
   server. The real 64-bit `libsteam_api.so` implements `SteamGameServer015` against these
   `SteamGameServer011` headers, so it is not a drop-in — updating ReHLDS's Steam integration
   is its own workstream and the brief's #1 risk-register item stands confirmed.
2. **No gameplay differential testing.** Movement, traces, spread and round logic have not
   been compared against the 32-bit reference. Note `mathlib_e.h:35-40` exists specifically
   to compensate for i386 x87 excess precision; on x86-64 that path is gone, so
   floating-point results *will* differ somewhere. Bit-identical cross-architecture gameplay
   is not an achievable acceptance criterion — tolerances must be defined per quantity.
3. **JIT and SSE are off** in this build. The portable delta path is already validated
   against the JIT by the existing tests (`11-portable-fallbacks.md`), but the 64-bit build
   has not been benchmarked against a JIT-enabled one.
4. **HLTV is unported** and excluded from 64-bit builds.
5. **No sanitizer run.** ASan/UBSan on the 64-bit build is the obvious next step and is
   likely to find more of §2's class of defect.

## 5. Incidental performance observation

Same 10-bot benchmark, `-pingboost 4`, `sys_ticrate 1000`:

| | 32-bit | 64-bit |
|---|---|---|
| frame execution p50 | 19.0 µs | **14.6 µs** |
| `SV_Physics` p50 | 13.0 µs | **10.3 µs** |

Roughly 25 % less CPU per frame — plausibly the extra registers and better codegen. **Do not
treat this as a result yet:** the runs were on different cores, the 64-bit build has JIT and
SSE disabled and no asmlib, and bot workloads vary run to run (`10-gameplay-ab.md` measured a
54 % within-condition spread on gameplay counters). It is a reason to measure properly, not a
measurement.
