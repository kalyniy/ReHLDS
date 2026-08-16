# Measurement 17 — UBSan and ASan on the 64-bit Build

**Date:** 2026-08-16
**Build:** `REHLDS_TARGET_BITS=64`, `DEBUG=ON`, JIT and SSE off
**Workload:** `de_dust2`, 10 zBots, `-pingboost 4`, ~2 minutes of gameplay per run

---

## 1. AddressSanitizer found a live stack-corruption bug

```
ERROR: AddressSanitizer: stack-buffer-overflow
WRITE of size 8
    #3 Host_UpdateStats() rehlds/engine/host_cmd.cpp:385
    #4 _Host_Frame(float)  rehlds/engine/host.cpp:963
```

`Host_UpdateStats()` parses `/proc/<pid>/stat` with a format string full of `%lu` and `%ld`
conversions, into variables declared `int32`:

```c
int32 dummy;
int32 ctime;
int32 stime;
int32 start_time;
...
fscanf(pFile, "%d %s %c %d %d %d %d %d %lu %lu ... %ld %ld ...",
       &dummy, statFile, (char *)&dummy, &dummy, ..., &ctime, &stime, ..., &start_time, ...);
```

`%ld`/`%lu` write `sizeof(long)` bytes — 4 on i386, **8 on LP64**. Every one of those ~30
conversions overran its 4-byte variable by four bytes.

**This is not a corner case.** `sv_stats` defaults to `"1"` (`host.cpp:67`) and
`_Host_Frame` calls `Host_UpdateStats()` once per second, so a 64-bit server corrupted its
own stack once per second, out of the box, from the first second of uptime. It is invisible
on i386, where `long` is 4 bytes and the code is correct.

### Fix

The discarded fields now use assignment suppression, and the three kept values are typed to
match their conversions:

```c
long ctime = 0, stime = 0;
unsigned long start_time = 0;
...
if (fscanf(pFile,
        "%*d %*s %*c %*d %*d %*d %*d %*d %*lu %*lu %*lu %*lu %*lu"
        " %ld %ld %*ld %*ld %*ld %*ld %*ld %lu",
        &ctime, &stime, &start_time) != 3) { ... }
```

Conversion positions are unchanged, so the values read are identical to before on both
architectures. Suppression also removes the old `%s`, which read the `comm` field
unbounded into `statFile` — the same buffer that held the path.

Noted but deliberately **not** changed: per `proc(5)`, `starttime` is field 22, while this
reads the 21st conversion (`itrealvalue`). That discrepancy is pre-existing, identical on
both architectures, and only skews the cosmetic CPU-percent statistic. Correcting it is a
behaviour change and does not belong in a memory-safety fix.

**After the fix: ASan reports zero errors** across 2 minutes of 10-bot gameplay.

## 2. ASan needed `RTLD_DEEPBIND` removed

ASan refuses to run at all when a library is `dlopen`ed with `RTLD_DEEPBIND`
(google/sanitizers#611) — the sanitized allocator in the main binary and the one the
deep-bound library resolves for itself end up mismatched. Both `dlopen` sites
(`engine/sys_dll.cpp`, `dedicated/src/sys_linux.cpp`) now drop the flag under sanitizers
only, via `REHLDS_SANITIZED`.

The nested `#if` in that macro is deliberate: GCC has no `__has_feature`, and the
preprocessor does not short-circuit function-like macros inside a `&&` chain, so the
one-line `defined(__has_feature) && __has_feature(...)` form fails to compile there.

## 3. UBSan: 18 findings, all one class

Every report is an unaligned access, and there were **no** integer overflows, invalid
shifts, null dereferences or out-of-bounds accesses across full 10-bot gameplay.

| site | what |
|---|---|
| `common.cpp:272,284,389` | `MSG_WriteShort/Word/Long` — `*(int16*)buf = …` into the network buffer at arbitrary byte offsets |
| `common.cpp:2542,2553` | `COM_Munge` — `pc = (int *)&data[i * 4]` over a buffer with no alignment guarantee |
| `wad.cpp:90-93` | `lumpinfo_t` entries read from a WAD file buffer |
| `model.cpp:1693-1705,1842` | `dspriteframe_t` / `dspriteframetype_t` from sprite file data |
| `sv_steam3.cpp:273` | misaligned load |

These are all **deliberate** unaligned accesses to packed on-disk or on-wire data through
typed pointers. Technically UB in C++; universally done in this codebase family.

**Practical risk, stated honestly:** lower than the count suggests. x86-64 handles unaligned
access in hardware, and **AArch64 Linux also permits unaligned normal loads and stores** —
it faults only for exclusive/atomic instructions and device memory. So these are unlikely to
break the ARM64 port directly.

The real exposure is the optimiser: a compiler entitled to assume alignment may emit vector
instructions that *do* require it. That makes these worth fixing before enabling aggressive
vectorisation or a new architecture, but they are not blockers, and none was fixed here —
changing 18 hot-path accesses without a benchmark would be optimising blind.

## 4. Status

| sanitizer | result |
|---|---|
| ASan | **clean** (was: 1 stack-buffer-overflow, fixed) |
| UBSan | 18 alignment reports, no other UB |

32-bit verified unaffected throughout: 33/33 unit tests, and a 10-bot run still reaches
998.5 Hz.

## 5. Not covered

- Neither sanitizer was run on the **32-bit** build. The `Host_UpdateStats` bug could not
  exist there, but others might.
- No real client connected, so the lag-compensation and studio-hull paths were exercised only
  as far as bots reach them (bots do not shoot through lag compensation —
  `09-hitreg-instrumentation.md` §1).
- No map changes, no `changelevel`, no custom content upload, no HLTV. The `.hpk` path in
  particular is known to differ across architectures and was only touched incidentally.
- ThreadSanitizer not run.
