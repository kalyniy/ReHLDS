# Audit 04 — x86-64 Portability

**Baseline commit:** `0124d56` (tag `baseline-x86-32`)
**Date:** 2026-08-15
**Method:** repo-wide source inspection. Load-bearing claims re-verified against the tree
before recording; two claims were corrected during verification and are marked as such.

Categories, per the brief: **(1)** build-naming only · **(2)** genuine ABI dependency ·
**(3)** x86 implementation choice with a portable fallback present · **(4)** external
binary dependency · **(5)** test/compat assumption.

---

## Headline

The port is **not** flag removal, but it is also not uniformly hard. The tree splits
cleanly:

- **Already portable, or portable with a switch:** all SSE/math (complete scalar twins,
  runtime `cpuinfo` dispatch, live-tested), the delta JIT (complete non-JIT path), the
  hook system (pure C++ templates, no trampolines), the savegame format (field-driven,
  not raw-struct), and the delta *wire* field tables (the structs they `offsetof` into are
  pointer-free).
- **Genuinely hard:** four things, listed below. Everything else is mechanical.

The single most useful early action is nearly free: **`REHLDS_SSE` and `REHLDS_JIT` are
unconditionally defined at `rehlds/CMakeLists.txt:259-260` with no CMake option to turn
them off**, despite both having complete, tested fallbacks. Exposing two options makes a
scalar build reachable without touching any engine source.

---

## The four real blockers

### B1 — `string_t` is a 32-bit pointer offset **(cat 2)**

```c
// rehlds/common/const.h:745
typedef unsigned int	string_t;
```
```c
// rehlds/engine/pr_edict.cpp:664-667
int EXT_FUNC AllocEngineString(const char *szValue)
{
	return ED_NewString(szValue) - pr_strings;
}
```

A pointer difference truncated into `int`, and this is the public `ALLOC_STRING` engine
export. The consumer side sign-extends:
```c
// rehlds/engine/pr_edict.cpp:607-610
const char* EXT_FUNC SzFromIndex(int iString)
{
	return (const char *)(pr_strings + iString);
}
```

Other truncating producers: `sv_main.cpp:1409`, `sv_main.cpp:6532,6542-6543`,
`pr_cmds.cpp:136,138,2017`. Strings live in a 128 KB hunk block
(`ed_strpool.cpp:53-54`) while `pr_strings` points at `gNullString`, a `.data` literal —
so on LP64 the distance between them is unbounded and the truncation is not merely
theoretical.

Two options, both consequential: place `pr_strings` in a dedicated sub-4 GB arena, or
widen `string_t` — which is an unavoidable GameDLL ABI break. **This is a design decision
for the owner, not something to improvise.**

### B2 — `edict_t` / `entvars_t` layout is the engine↔GameDLL contract **(cat 2)**

`edict.h:18-33` embeds `link_t area` (2 pointers), `void *pvPrivateData`, then `entvars_t v`
**by value**. `entvars_t` (`progdefs.h:141-146,181,217-220`) contains 11 `edict_t*` members
(`chain`, `dmg_inflictor`, `enemy`, `aiment`, `owner`, `groundentity`, `pContainingEntity`,
`euser1..4`). Every field after each pointer shifts on LP64.

Nothing links until the GameDLL is rebuilt against the identical LP64 layout, and every
existing third-party plugin binary breaks. Unavoidable, and it is the reason the brief
correctly sequences ReGameDLL alongside the engine rather than after it.

### B3 — Non-PIC shared libraries **(cat 2)**

```cmake
rehlds/CMakeLists.txt:42   set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "")
rehlds/CMakeLists.txt:324      POSITION_INDEPENDENT_CODE OFF
```
Same pattern in `HLTV/Director/CMakeLists.txt:119` and `HLTV/Proxy/CMakeLists.txt:155`.

Building a `.so` without `-fPIC` merely works by accident on i386. On x86-64 it **fails to
link** (`R_X86_64_32` / `R_X86_64_32S` relocations against `.text`). This must be reverted
before anything compiles — and reverting it may perturb whatever performance assumption
motivated it, so measure before and after.

### B4 — `.hpk` on-disk format carries host pointer width **(cat 2)**

```c
// rehlds/engine/hashpak.cpp:382
FS_Write(&newdirectory.p_rgEntries[j], sizeof(hash_pack_entry_t), 1, iWrite);
```
`hash_pack_entry_t` embeds `resource_t`, which ends in two pointers
(`rehlds/public/rehlds/custom.h:78,81`):
```c
	struct resource_s *pNext;
#if !defined(HLTV)
	struct resource_s *pPrev;
#endif
```
Read side identical at `hashpak.cpp:105,322,488,584,650`.

A 64-bit build would silently write `.hpk` custom-content paks that no existing HLDS can
read, and misread every existing one. Needs an explicit fixed-width serializer plus a
format-version bump.

**Notable:** the savegame path turned out *clean* — `host_cmd.cpp:129-164` drives it
through field-descriptor tables, not raw structs. The brief asked about save files; the
leak is in the custom-content pak instead.

---

## Delta JIT — not a port prerequisite, confirmed

The portable path is complete and self-consistent. The selecting symbol is `REHLDS_JIT`;
the portable path is taken when it is **not** defined (`rehlds/CMakeLists.txt:259`, `:279`).

Every JIT entry point has a C++ sibling under `#else` — verified pairs at `delta.cpp`
`391/394`, `400/403`, `422-423/425-499`, `615-616/618`, `755-756/758-760`, `770-771/773-775`,
`784-785/789-806`, `812-813/815`, `821-822/824-833`, `846-847/849`. Mask bookkeeping the JIT
would otherwise do is supplied at `delta.cpp:415-417` and `:574-577`, gated
`#if defined REHLDS_FIXES && !defined REHLDS_JIT`. All of `delta_jit.cpp` and the three
registration sites (`sv_main.cpp:8171,8217,8436`) are inside `#ifdef REHLDS_JIT`.

**`REHLDS_JIT` off + `REHLDS_FIXES` on is a complete build.** This validates the brief's
plan to disable the JIT for first 64-bit/ARM64 bring-up.

The codegen itself is thoroughly i386: fixed `esi`/`edi`/`ebx`/`ebp` allocation, `ah` byte
registers, 4-byte stack slots, x87 `fld`/`fstp`, and — the clearest example —
`mov(ecx, (size_t)&Q_stricmp); push(eax); push(edx); call(ecx);`
(`delta_jit.cpp:339-342`, also `:584-587`), which truncates a 64-bit function address into
a 32-bit register.

**Unanticipated:** `jitasm` itself is *already* 64-bit capable — `jitasm.h:38-39` defines
`JITASM64` for `__x86_64__`, and `jitasm.h:7556-7600` implements `ArgTraits_linux64`, a
full SysV AMD64 argument classifier. Only ReHLDS's ~450 lines of hand-written codegen are
i386-only. A future AArch64 backend would need jitasm work; an x86-64 one would not.

---

## Corrections made during verification

Two claims from the sweep did not survive checking, and are recorded so they are not
carried forward as fact:

1. **`structSizeCheck.cpp` is *not* a build-time blocker.** The `client_t` assertion is
   commented out, and the `CSteam3Server` one is behind `#ifndef REHLDS_FIXES` — and
   `REHLDS_FIXES` *is* defined (`CMakeLists.txt:261`). The only live assertion is
   `CHECK_TYPE_SIZE(userfilter_t, 0x20, 0x18)`, and `userfilter_t`
   (`rehlds/engine/filter.h:48-53`) is `USERID_t` + two `float`s with no pointers, so it is
   LP64-stable. Nothing here aborts the build.
2. **`unittests/struct_offsets_tests.cpp` is a real issue but not a *blocker*** — it is a
   runtime test, not a compile-time abort. Its seven `CHECK_STRUCT_OFFSET` assertions on
   `client_t` (`:23-29`) pin the layout of Valve's original binary so third-party plugins
   that hard-code offsets keep working. `client_t` (`server.h:181-240`) embeds `netchan_t`
   (4+ pointers), `client_frame_t *frames`, `edict_t *edict`, two `resource_t`, and more —
   so every one of these offsets *legitimately* changes on LP64. The test must be made
   arch-conditional. **It is not protecting a serialized format**, and the real cost it
   signals is loss of binary compatibility with the Metamod/AMXX ecosystem — which the
   brief already sequences as Phase 8.

---

## Things the brief did not anticipate

### The companion repo already solved the build-system half

`ReGameDLL_CS/regamedll/CMakeLists.txt:47-58` gates `-m32` on `CMAKE_SIZEOF_VOID_P EQUAL 8`
**and** an `XASH_COMPAT` option, and `regamedll/cmake/LibraryNaming.cmake:118-126`
implements the Xash3D-FWGS naming scheme (`BUILDARCH` ∈ `amd64`/`i386`/`arm64`/…), with
arch detection in `regamedll/public/build.h:183-186`. ReHLDS has none of this. The pattern
to copy — **and the filename convention the GameDLL already expects to find the engine
under** — is sitting in the reference repo.

### `SO_ARCH_SUFFIX` already exists in ReHLDS and is dead

`rehlds/common/port.h:112-116` defines `"_amd64.so"` for `__x86_64__`. Grep finds the three
definition lines and **zero uses**. Meanwhile `rehlds/public/engine_hlds_api.h:36` hard-codes
`#define ENGINE_LIB "engine_i486.so"`, which is what the dedicated launcher `dlopen`s.
Someone started this work and stopped.

### `typedef unsigned long DWORD` diverges *between* 64-bit platforms

`osconfig.h:152` gives Linux an 8-byte `DWORD` on LP64 while Win64 keeps it at 4. This is a
new cross-platform size *divergence* introduced by porting, not a simple widening.

### x87 → SSE changes floating-point results

`mathlib_e.h:35-40`: `#if !defined(REHLDS_FIXES) && !defined(REHLDS_SSE) typedef double real_t;`
exists specifically to compensate for i386 x87 excess precision. On x86-64 the x87 path is
gone by default. Results will shift. This directly concerns the brief's differential
contract (§19.2) and demo-playback determinism, and it means **"bit-identical gameplay
across architectures" is not an achievable acceptance criterion** — tolerances must be
defined per quantity, as the brief's §12.3 anticipates but does not quantify.

### Pre-existing bug the JIT is currently hiding

Unrelated to portability, but it matters *because* the port turns the JIT off. The
non-JIT mask bookkeeping at `delta.cpp:576`, `:798`, `:830` indexes `u32[i >> 5]` into a
2-word `delta_marked_mask_t` (`delta.h:75-79`) with no bound on `fieldCount`. The JIT path
caps this at `DELTAJIT_MAX_FIELDS = 56` (`delta_jit.h:35`, enforced at
`delta_jit.cpp:90-92`); the portable path has no equivalent check, so a `delta.lst` with
more than 64 fields overruns the mask. **Add the bound check before disabling the JIT.**

### `NOXREFCHECK` fails silently rather than loudly

`maintypes.h:55`: `__asm__ __volatile__("movl 16(%%esp), %%eax; …")` — a hard-coded i386
stack offset used by every `NOXREF` body. It compiles cleanly on x86-64 and reads garbage,
so the diagnostic silently lies. Disproportionately expensive to find later.

### Other unanticipated items

- `rehlds/public/strtools.h:67-107` maps `Q_memcpy`→`A_memcpy` etc. under
  `HAVE_OPT_STRTOOLS`; the `#else` at `:108+` is a **complete libc fallback**. Dropping
  `libaelf32.a` is a one-define change (cat 3, trivial) at a measurable — and measurable
  is the point — performance cost on the hottest string paths.
- `rehlds/dedicated/src/isys.h:42`: `virtual long LoadLibrary(const char *lib) = 0;` —
  a module handle as `long` in a **virtual interface**. Truncates on Win64 (where `long`
  is 32-bit), works on LP64 Linux. Implementations at `sys_linux.cpp:217,245-250`.
- `rehlds/rehlds/rehlds_messagemngr_impl.cpp:257-268`: `#pragma pack(push,1)` over
  bitfields declared with a `size_t` base type. On LP64 the allocation unit becomes 8
  bytes and `sizeof(Param_t)` changes.
- `rehlds/public/utlbuffer.cpp:248,258,268,278`: `m_Get = (int)pEnd - (int)Base();` — two
  independent pointer→`int` truncations, ×4 sites.
- `rehlds/engine/common.cpp:333-357,371`: `ALIGN16 bf_write_t bfwrite;` survives LP64 only
  because `sizebuf_t *pbuf` happens to sit after the SSE-accessed members. Fragile
  invariant — add a `static_assert`.
- `rehlds/HLTV/common/DemoFile.cpp:95-100` writes `m_loadEntry` / `m_gameEntry` /
  `m_demoHeader` as raw structs into `.dem`. A second potential format leak; those three
  structs were **not** exhaustively layout-audited and should be.
- `rehlds/testsuite/` (not in the CMake build, MSVC-only) contains a hard-coded ELF32
  symbol walker (`memory.cpp:288-374`) and PE IAT patching with a 32-bit truncating cast
  (`testsuite.cpp:240-244`). Out of scope now; will need attention if the record/replay
  harness is ever revived — which the test strategy in brief §19 might want.

---

## Recommended first portability steps (cheap, non-invasive, high information)

1. Add `option(REHLDS_ENABLE_JIT ...)` and `option(REHLDS_ENABLE_SSE ...)`, defaulting ON.
   Zero behaviour change; makes a scalar build reachable.
2. Add the `delta.cpp` mask bound check (guards the fallback the port depends on).
3. Build x86-32 with **JIT off, SSE off** and confirm all 33 unit tests still pass. This
   validates the fallbacks *before* architecture is added as a second variable — exactly
   the brief's "one variable at a time" rule.
4. Add a delta encode/decode equivalence test: same vectors through JIT and portable paths,
   compare byte-for-byte. This does not exist today and is a prerequisite for trusting the
   portable path.
5. Only then introduce a 64-bit build target, starting with B3 (PIC), since nothing links
   until it is fixed.
