# Portable Fallbacks — Selectable, and Validated on x86-32

**Date:** 2026-08-16
**Branch:** `audit/m0-baseline`

The brief's "one variable at a time" rule (§4) says the portable paths should be proven
*before* architecture becomes a second variable. Until now they could not even be selected:
`REHLDS_JIT` and `REHLDS_SSE` were defined unconditionally in `rehlds/CMakeLists.txt`.

---

## 1. Two new options, defaults unchanged

```cmake
option(REHLDS_ENABLE_JIT "Build the x86 delta JIT. OFF selects the portable C++ delta path." ON)
option(REHLDS_ENABLE_SSE "Build the SSE mathlib paths. OFF selects the scalar implementations." ON)
```

Both default `ON`, so the shipped configuration is byte-for-byte the same set of defines as
before. The `Unittests` configuration previously re-asserted `REHLDS_SSE REHLDS_JIT`
regardless; that has been folded into the same options so the test build follows the
selection instead of overriding it.

## 2. Result: the scalar / no-JIT build works

| configuration | build | tests |
|---|---|---|
| default (JIT on, SSE on), Unittests | OK | **33 passed** |
| `REHLDS_ENABLE_JIT=OFF REHLDS_ENABLE_SSE=OFF`, Unittests | OK | **32 passed** |
| `REHLDS_ENABLE_JIT=OFF REHLDS_ENABLE_SSE=OFF`, release `.so` | OK | — |

The single test difference is `MathLib::Length2DTest`, which is SSE-only by design (§4).
Everything else, **including all five delta tests**, runs and passes in both.

## 3. Two real gaps found — both in the test suite, not the engine

The scalar build did not link at first. The engine code turned out to be correctly guarded
throughout; the breakage was in `unittests/`, which referenced SSE-only symbols
unconditionally:

- `crc32c_tests.cpp:32,35` called `crc32c_t_sse`, which exists only inside
  `#ifdef REHLDS_SSE` (`crc32c.cpp:117-138`). The pure-table reference checks are now
  unconditional and the SSE comparison is guarded, so the scalar build still validates the
  known-good checksums.
- `mathlib_tests.cpp:175` called `Length2D`, which is defined **only** in
  `mathlib_sse.cpp:367`.

Neither would have been noticed until an x86-64 or ARM64 build was attempted, at which
point they would have looked like architecture problems rather than what they are.

## 4. `Length2D` is an SSE-only entry point, and now says so

`Length2D` was declared unconditionally in `mathlib_e.h:163` but defined only in
`mathlib_sse.cpp`. Its sole engine call site already guards itself and has a scalar twin:

```c
// rehlds/engine/mathlib.cpp:169-173
#ifdef REHLDS_SSE
        length = Length2D(forward);
#else
        length = Q_sqrt((double)(forward[0] * forward[0] + forward[1] * forward[1]));
#endif // REHLDS_SSE
```

The declaration is now guarded too, so using it in a scalar build is a compile error at the
call site rather than a link error at the end of the build.

## 5. Correction: the JIT/portable equivalence test already exists

`02-build-reproduction.md` recorded, on the strength of the portability sweep, that no test
compares the JIT and portable delta paths, and logged writing one as a port prerequisite.
**That was wrong.**

```c
// rehlds/unittests/delta_tests.cpp:243
for (int usejit = 0; usejit <= 1; usejit++) {
```

`_DeltaSimpleTests` drives every vector through `_DoMarkFields` with `useJit` both false and
true, checking both against the same expectations (`delta_tests.cpp:65-79`, `:236-246`). In
a JIT build the portable path is therefore already validated against the JIT on every run,
across all four delta test groups.

This is a meaningful de-risking of the port: the path an x86-64 or ARM64 build falls back to
is not untested code.

## 6. Correction: the delta mask overrun is not reachable

The sweep also reported that the portable mask bookkeeping indexes `u32[i >> 5]` into a
2-word `delta_marked_mask_t` with no bound on `fieldCount`, and that the JIT's
`DELTAJIT_MAX_FIELDS` check is what currently hides it — implying that disabling the JIT
would expose an overrun.

Checked, and it does not:

- Every mask site is inside `REHLDS_FIXES`: `delta.cpp:575-577` is
  `#if defined REHLDS_FIXES && !defined REHLDS_JIT`, and `DELTA_WriteDeltaForceMask` /
  `DELTA_GetOriginalMask` / `DELTA_GetMaskU64` (containing `:798` and `:830`) sit inside a
  single `#ifdef REHLDS_FIXES` block.
- The cap is enforced under the *same* condition — `delta.cpp:1161-1163`,
  `#ifdef REHLDS_FIXES`, `Sys_Error` if `count > DELTA_MAX_FIELDS`, with
  `DELTA_MAX_FIELDS = 56` (`delta.h:33`).

56 < 64, so `i >> 5` is always 0 or 1 and in bounds. Whenever the mask code is compiled, the
cap is compiled with it. No change needed.

## 7. Running total of corrections

Three claims from the automated portability sweep have now failed verification and been
corrected in place rather than acted on:

1. `structSizeCheck.cpp` static asserts are a build blocker — **inert** (`client_t` check is
   commented out; `CSteam3Server` is behind `#ifndef REHLDS_FIXES`, which is defined).
2. No JIT/portable delta equivalence test exists — **it exists** (§5).
3. The portable delta mask can overrun — **unreachable** (§6).

The sweep remains useful; its inventory of `-m32`, ABI, serialization and inline-asm sites
has held up well. But its *severity judgements* have a poor hit rate, which is worth
remembering when planning from it. Verify before acting.

## 8. What this does not establish

- The scalar build passes the same unit tests, but **no gameplay differential** has been run
  between a JIT/SSE build and a scalar one. Floating-point results can legitimately differ
  between SSE and x87/scalar paths (`mathlib_e.h:35-40` exists precisely to compensate for
  i386 x87 excess precision), so movement, traces and spread should be compared before the
  scalar configuration is used for anything but porting work.
- No performance measurement of the scalar configuration. The point here was correctness and
  buildability, not cost. If the scalar path is materially slower, that matters for ARM64
  planning and is unmeasured.
