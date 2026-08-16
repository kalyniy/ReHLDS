# M0 — x86-32 Baseline Build Reproduction

**Result: reproduced. Release and Unittests configurations both build; all 33 unit tests pass.**

**Commit:** `0124d56c3d888d922eb045775f71c6682ad1226f` (tag `baseline-x86-32`)
**Host:** i9-12900K / Ubuntu 24.04.4 / kernel 6.17.0-22-generic
**Toolchain:** GCC 13.3.0, CMake 4.2.1, glibc 2.39, binutils via Ubuntu 24.04

---

## 1. Two toolchain incompatibilities had to be resolved

Neither is a defect in ReHLDS. Both are consequences of the CI baseline (Debian 11,
glibc 2.31, CMake 3.18) being older than this host. Both are recorded because they will
recur for anyone reproducing this work, and because the second one is a genuine
dependency-track finding.

### 1.1 CMake 4.x rejects `cmake_minimum_required(VERSION 3.1)`

```
Compatibility with CMake < 3.5 has been removed from CMake.
```
CMake 4.0 dropped support for policy versions below 3.5. All `CMakeLists.txt` in the
tree declare `cmake_minimum_required(VERSION 3.1)`.

**Handled without editing the source**, via `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, so the
baseline remains byte-identical to upstream. Raising the declared minimum in-tree is a
reasonable future cleanup but is deliberately *not* part of the frozen baseline.

### 1.2 The vendored `lib/linux32/librt.so` cannot link on glibc >= 2.34

```
/usr/bin/ld: rehlds/lib/linux32/librt.so: undefined reference to `__libc_dlopen_mode@GLIBC_PRIVATE'
/usr/bin/ld: rehlds/lib/linux32/librt.so: undefined reference to `__libc_dlsym@GLIBC_PRIVATE'
/usr/bin/ld: rehlds/lib/linux32/librt.so: undefined reference to `__pthread_unwind@GLIBC_PRIVATE'
```

`rehlds/lib/linux32/librt.so` is an old import stub (`for GNU/Linux 2.6.15`, stripped)
vendored so that release binaries bind to ancient glibc symbol versions and therefore run
on legacy server distros — this is what `rehlds/version/glibc_test.sh` enforces in CI. It
imports three `GLIBC_PRIVATE` symbols that **were removed in glibc 2.34**, when
`librt`/`libpthread`/`libdl` were folded into `libc`. Any host with glibc >= 2.34
(Ubuntu 22.04+, Debian 12+) therefore cannot link the engine at all.

The vendored `.so` wins over the system one because
`-L${PROJECT_SOURCE_DIR}/lib/linux32` is prepended to the link line.

**Fix applied** — a new CMake option, defaulting to the existing behaviour:

```cmake
option(USE_VENDORED_COMPAT_LIBS "..." ON)
```
When `OFF`, `rt` and `m` are linked as `-l:librt.so.1` / `-l:libm.so.6`, which matches the
exact sonames and so bypasses the vendored stubs; `aelf32` and `steam_api` continue to
resolve from `lib/linux32` unchanged. Default `ON` preserves upstream behaviour exactly,
so release builds and `glibc_test.sh` are unaffected.

**Trade-off, stated explicitly:** binaries built with `OFF` require the host's glibc
(2.39 here) and are **not** suitable for public release. They are for local development,
benchmarking, sanitizer and profiling builds — which is all this project needs from them
until Phase 8. A release build must either run on a legacy container or replace the
vendored stubs properly. **This belongs on the dependency workstream, not the core
workstream.**

## 2. Exact reproduction commands

```bash
sudo apt-get install -y gcc-multilib g++-multilib libc6-dev-i386
```

Unit tests:
```bash
cd /home/dan/projects/ReHLDS
rm -rf build
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUSE_VENDORED_COMPAT_LIBS=OFF \
      -DCMAKE_BUILD_TYPE=Unittests -B build
cmake --build build -j24
LD_LIBRARY_PATH="rehlds/lib/linux32:$LD_LIBRARY_PATH" ./build/rehlds/engine_i486
```

Release:
```bash
rm -rf build-release
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUSE_VENDORED_COMPAT_LIBS=OFF -B build-release
cmake --build build-release -j24
```

## 3. Results

### Unit tests — 33/33 passed, exit code 0

```
There were no test failures; Tests executed: 33
```

Coverage by area, as reported by the runner:

| Area | Tests |
|---|---|
| Delta encoding (incl. JIT path) | 5 — `TestDelta_Test`, `MarkFieldsTest_{TimeWindow,Strings,InterBlock,Simple_Primitives}` |
| Mathlib (SSE path active) | 12 — concat/VectorMA/compare/angles/normalize/length/cross/dot/AngleMatrix/… |
| Info strings | 9 |
| Unicode / tmessage | 3 |
| Struct offsets | 1 — `ReversingChecks::StructOffsets` |
| Security (file extension) | 1 |
| CRC32C | 1 |
| MSG bit I/O | 1 |

Two facts worth carrying forward:
- `sizeof server_t: 0x4640C` (287,244 bytes) and `sizeof CSteam3Server: 0x128` are printed
  by the offsets test. These are **in-memory ABI**, not wire format, and *will* change on
  LP64. The struct-offsets test is therefore an x86-32 ABI guard, not a protocol guard —
  it must be re-based, not merely made to pass, during the x86-64 port.
- ~~The delta tests exercise the **JIT** path only; there is no portable-path equivalence
  test.~~ **CORRECTED (see `11-portable-fallbacks.md`).** `_DeltaSimpleTests`
  (`unittests/delta_tests.cpp:243`) loops `for (int usejit = 0; usejit <= 1; usejit++)`, so
  every delta vector is already run through **both** the JIT and the portable C++ path and
  checked against the same expectations. The equivalence test exists and passes. This
  materially de-risks the x86-64/ARM64 port, since the path those builds fall back to is
  already validated against the JIT.

### Release build — succeeded, all artifacts produced

```
build-release/rehlds/engine_i486.so
build-release/rehlds/dedicated/hlds_linux
build-release/rehlds/filesystem/FileSystem_Stdio/filesystem_stdio.so
build-release/rehlds/HLTV/{Console/hltv,Core/core.so,DemoPlayer/demoplayer.so,Director/director.so,Proxy/proxy.so}
```

`engine_i486.so`: ELF 32-bit LSB shared object, Intel 80386, dynamically linked.
Zero compiler warnings at the project's configured warning level (which suppresses a
large set — see below). Two linker warnings, both pre-existing and expected:

```
libaelf32.a(memcpy32.o32): warning: relocation in read-only section `.text'
warning: creating DT_TEXTREL in a PIE
```
These come from the vendored hand-written 32-bit assembly memcpy in `libaelf32.a` — a
non-PIC text relocation. Noted for the portability track: `libaelf32.a` is a
**category-4 external binary dependency** (x86-32 assembly, no source in-tree) and has no
x86-64 or AArch64 equivalent here.

### Reference binaries archived

`/home/dan/projects/rehlds-reference/x86-32-gcc13/`

```
e5894a4e44594b8b901f72c2dcd9418693f248fd0b2c14527833d1163356f28f  engine_i486.so
dca78cc23161ce8783198d204c38400625eddfa3d697066572e6ec5472e7680a  filesystem_stdio.so
41987fa06963c032f320221f52f657c7eb979aad01ccec681542ee4a05dae2c8  hlds_linux
```

Caveat: these are **not** bit-reproducible across rebuilds — `rehlds/version/appversion.sh`
stamps commit metadata into the binary, and the build is not otherwise hardened for
reproducibility (no `-frandom-seed` pinning, no `SOURCE_DATE_EPOCH` handling). They are a
fixed *reference artifact*, not a reproducibility proof. If bit-identical rebuilds become
necessary for differential work, that is separate work to scope.

## 4. Warning suppression — relevant to the x86-64 port

The build explicitly silences much of what a portability audit would want to see:

```
-Wno-invalid-offsetof -Wno-sign-compare -Wno-strict-aliasing -Wno-ignored-attributes
-Wno-write-strings -Wno-format -Wno-class-memaccess -Wno-unused-* -fpermissive
```

`-Wno-format` and `-Wno-class-memaccess` in particular hide exactly the defect classes
(printf width mismatches on `long`/pointer, `memcpy` over non-trivial types) that break
on LP64. Per brief §12.4, a separate **portability-warnings preset** should re-enable
these without forcing the legacy tree clean in one step. Logged as a Phase 5/6 task; not
changed now, because changing warning flags now would perturb the frozen baseline.

## 5. Deviations from CI, recorded

| Item | CI | Here | Impact |
|---|---|---|---|
| Distro | Debian 11 (glibc 2.31) | Ubuntu 24.04 (glibc 2.39) | forced §1.2 |
| CMake | 3.18-era | 4.2.1 | forced §1.1 |
| GCC | Debian 11 default (10.x) | 13.3.0 | **different codegen — see below** |
| `-j` | 8 | 24 | none |
| Vendored compat libs | ON | OFF | higher min glibc; dev-only binaries |

The GCC version difference is the one that matters for gameplay differential testing: CI's
GCC 10 and this host's GCC 13 will not produce identical floating-point code sequences in
every case. Since the project's differential contract (brief §19.2) requires comparing
candidate builds against a reference, **the reference must be the GCC 13.3.0 build made
here**, not CI's artifacts. Comparing across compiler versions is a separate, later
experiment (Phase 5), not a baseline.
