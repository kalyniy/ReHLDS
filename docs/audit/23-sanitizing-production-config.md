# Measurement 23 — Sanitizing the Configuration That Actually Ships

**Date:** 2026-08-16
**Build:** 32-bit, `REHLDS_ENABLE_JIT=ON`, `REHLDS_ENABLE_SSE=ON` — i.e. the production config
**Tools:** UBSan (`-fsanitize=undefined`), ASan (`-fsanitize=address -fsanitize-recover=address`)
**Load:** `de_dust2`, 9–10 zBots, `-pingboost 4`, `sys_ticrate 1000`, pinned `taskset -c 2`

---

## 1. The gap this closes

Every sanitizer run before this one (`docs/audit/17`) was against the **64-bit** build. That
build has `REHLDS_ENABLE_JIT=OFF` and `REHLDS_ENABLE_SSE=OFF`, because the delta JIT is an
IA-32 code generator and the SSE mathlib was disabled during bring-up.

So the two subsystems that are **hottest in the shipping binary** — the delta JIT and the SSE
mathlib — had never been sanitized at all. Both defects below are invisible to a 64-bit run
by construction, which is exactly why they survived.

## 2. UBSan: two new defects on the production path

23 reports. 21 are the already-documented unaligned-access class from `docs/audit/17`. Two
were new:

**`rehlds/jitasm.h:890` — `memset(NULL, 0xCC, 0)`.** `Backend`'s default constructor memsets
its buffer, and `ResolveJump()` deliberately default-constructs one with `pbuff = NULL` to
measure code size before allocating. So this ran on **every delta JIT creation — every server
start**. Zero length makes it harmless in practice, but `memset`'s first parameter is declared
non-null, and both GCC and Clang are entitled to assume the pointer is non-null *afterwards*
and delete later null checks. Fixed by guarding the call.

**`public/rehlds/crc32c.cpp:134` — a misaligned load, and a real loop bug behind it.**
Investigating the alignment report surfaced something worse in the same line:

```c
for (; i < (len >> 2); i += 4)          // i is a BYTE index; (len >> 2) is a WORD count
    crc32cval = _mm_crc32_u32(crc32cval, *(uint32*)&buf[i]);
```

For `len = 64` the bound is 16, so the SSE path consumed bytes 0–15 and left 16–63 to the
byte-at-a-time loop below it. **The fast path was doing a quarter of its intended work.**

Output was never wrong, which is why the unit tests pass identically before and after: CRC32C
is sequential, and the byte loop resumes at exactly the index the word loop stopped at. A
correctness test cannot see this bug — only reading the loop can. Fixed the bound to
`i + 4 <= len` and replaced the typed load with `memcpy`.

## 3. ASan: a 16-byte read of every 12-byte vector

ASan halted on the **first map load**:

```
ERROR: AddressSanitizer: stack-buffer-overflow
READ of size 16 at 0xf1d6d0c0
    #0 Length(float const*)
    #1 Mod_LoadBrushModel_internal(model_s*, void*)
    ...
  [64, 76) 'corner' (line 1256) <== Memory access at offset 64 partially overflows this variable
```

`corner` is a `vec3_t` — 12 bytes. `Length()` read 16.

The cause is systemic, not local. `mathlib_sse.cpp` loads vectors with `_mm_loadu_ps`, which
always moves 16 bytes, and hands it a `vec3_t` in **11 places**:

| function | profile share (`docs/audit/22` §2) |
|---|---|
| `VectorMA` | **2.25 %** |
| `BoxOnPlaneSide` | (inlined into BSP traversal) |
| `AngleVectors` ×3 | |
| `_DotProduct`, `Length`, `Length2D`, `CrossProduct` | |

Note the asymmetry that made this survive: the **store** helper is careful —

```c
inline void xmm2vec(vec_t *v, const __m128 m) {
    _mm_storel_pi((__m64*)v, m);            // 8 bytes
    _mm_store_ss(v + 2, ...);               // + 4 = exactly 12
}
```

The author knew a `vec3_t` is 12 bytes and wrote a 12-byte store. There was simply no
matching helper on the load side, so `_mm_loadu_ps` got used directly everywhere.

**Why it has been benign.** A `vec3_t` is nearly always embedded in a larger struct
(`entvars_t`, `edict_t`) or a stack frame, so the 4 extra bytes are readable padding. It
faults only when a vector ends exactly on a page boundary. That is a latent hazard, not a
present crash — but it is also *undefined behaviour on the hottest math in the engine*, and
the garbage pulled into lane 3 can be a denormal, which slows the very operations it feeds.

### The fix

A `vec2xmm` that mirrors `xmm2vec`, touching exactly 12 bytes:

```c
inline __m128 vec2xmm(const vec_t *v) {
    return _mm_movelh_ps(_mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)v), _mm_load_ss(v + 2));
}
```

**This preserves results exactly.** Every consumer already discards lane 3 — `_mm_dp_ps` masks
it out via `0x71`, `xmm2vec` stores only three floats, `crossProduct3D`'s
`_MM_SHUFFLE(3,0,2,1)` keeps lane 3 in lane 3, `length2D` reads lane 0, and `BoxOnPlaneSide`
parks it in an unused `d1[3]`. Replacing garbage with a deterministic zero cannot change any
observable value, and makes results *reproducible* where they previously depended on whatever
memory followed the vector.

`VectorTransform`'s three loads were **left alone**: they read `float (*)[4]` matrix rows,
where 16 bytes is exactly in bounds.

### Validation

- All 12 `MathLib::*` tests pass. These matter here because `mathlib_tests.cpp` forces
  `cpuinfo.sse4_1 = 0` to exercise the non-`dp_ps` twin, and compares SSE output against the
  scalar implementations — so both sub-paths were checked against an independent oracle.
- **ASan: 0 errors** across 110 s with 9 bots, on the full JIT + SSE production config. The
  earlier run could not get past map load.

### What it costs — and an unexpected result

`vec2xmm` is three instructions where `_mm_loadu_ps` is one, so the expectation was a small
regression accepted in exchange for correctness. Measuring it took two attempts.

**The whole-server A/B could not resolve it.** Six interleaved 25 s runs (A,B,A,B,A,B), 10
bots, pinned, `sys_ticrate 1000`:

| run | p50 | p95 | p99 |
|---|---|---|---|
| A_run1 / A_run2 / A_run3 | 18.6 / 17.9 / 17.7 | 46.7 / 81.8 / 54.5 | 102.4 / 148.9 / 117.9 |
| B_run1 / B_run2 / B_run3 | 18.6 / 4.1 / 17.8 | 69.2 / 54.9 / 51.8 | 141.9 / 112.9 / 102.2 |

**A's own p95 spread (46.7 → 81.8 µs, a 75 % swing) is larger than any A-vs-B gap**, and B's
range sits inside A's. This is the outcome `gameplay_ab.sh` was written to make visible: the
frame tail is GameDLL bot AI, whose variance swamps two instructions. Two methodology notes:
the `B_run2` p50 of 4.1 µs is a *sampling* artifact, not a workload one — edict counts and
admitted frames match A_run2 exactly, but the frameperf ring holds only the **last 8.2 s** of
each 25 s window and can land on a round transition; and consequently every number above is an
8.2 s sample of a much longer round cycle.

**An isolated microbenchmark showed the opposite of the expected sign.** Same three functions,
32-bit, `-O2 -msse3`, verified bit-identical output first (0 mismatches over 65 535 vectors):

| | A (`_mm_loadu_ps`) | B (`vec2xmm`) |
|---|---|---|
| time | 304.7 ms | **75.6 ms** |
| instructions | 850.7 M | 1111.0 M (**+31 %**) |
| IPC | 0.57 | **2.98** |
| `ld_blocks.store_forward` | **26,225,770** | **5,703** |

B retires 31 % *more* instructions and finishes in a quarter of the cycles.

The first hypothesis — cache-line splits, since a 12-byte stride makes a 16-byte load cross a
64-byte line whenever `12i mod 64 > 48` — was **wrong**. A control run padding every vector to
a 16-byte stride, where splits are impossible, left the ratio unchanged (304.6 vs 75.5 ms) and
`split_loads` at 771 vs 532. The real mechanism is **store-to-load forwarding**:

```c
xmm2vec(out, ...);        // 8-byte store + 4-byte store
... Length(out) ...       // A: one 16-byte load  -> cannot forward
                          // B: 8-byte + 4-byte load -> forwards exactly
```

A load wider than the stores feeding it cannot be forwarded, so the core must drain those
stores to L1 first. That is one stall per iteration — 26.2 M stalls over 26.2 M iterations,
matching exactly — and it disappears when the load pattern mirrors the store pattern.

**Scope this honestly.** The microbenchmark writes a vector and immediately re-reads it every
iteration, which is the worst case for A; the engine does that only when vector operations are
chained. The truthful statement is that the fix removes a stall class that is real and large
where it occurs, is **below the noise floor of whole-server measurement**, and was adopted for
correctness rather than for speed. Nothing here contradicts `docs/audit/22`: the server still
uses 2–4 % of one core, and the mathlib is a small slice of that.

## 4. Why `VectorMA` was not simply inlined

`VectorMA` shows up as a *symbol* at 2.25 %, which normally means a tiny function is failing
to inline. It is: `mathlib_e.h:94` defines an inline template, but it sits under `#ifndef
REHLDS_FIXES`, so in the shipping build the only declaration is the out-of-line one at
`mathlib_e.h:156`. Every call is a real call around three multiply-adds.

**I did not inline it**, because doing so is not the free win it appears to be. The build sets
`-msse3` but never `-mfpmath=sse`, and i386 GCC defaults to **x87**, which evaluates at 80-bit
intermediate precision. The scalar `mathlib.cpp` twin and the SSE `mathlib_sse.cpp` version
therefore do **not** produce bit-identical results, and `mathlib.cpp:207-420` is
`#if !defined(REHLDS_SSE)` — so which one you get depends on build flags. Changing inlining
risks changing which rounding applies, and this is the math under hit registration. That is a
deliberate-decision change, not a cleanup, and it is not worth making for 2.25 % of a process
using 2–4 % of one core.

## 5. What this does not establish

- **Bots never exercise lag compensation.** `SV_RunCmd` gates `SV_SetupMove` on
  `!host_client->fakeclient`, so the ASan run did *not* cover the rewind path or the
  `sv_rehlds_unlag_pose` restore added in `docs/audit/21`. That path remains unsanitized and
  is the most valuable place to point ASan next, with a real client.
- The 21 remaining UBSan reports are unchanged and still unaddressed (`docs/audit/17`).
- ASan perturbs timing heavily; nothing here is a performance measurement.
- Still no gameplay differential testing against `baseline-x86-32`.
