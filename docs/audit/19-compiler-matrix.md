# Measurement 19 — Compiler Matrix: Inconclusive, and Why That Is the Answer

**Date:** 2026-08-16
**Builds:** GCC 13.3.0 `-O3` · Clang 18.1.3 `-O3` · Clang 18.1.3 `-O3 -flto=thin`
**Workload:** `de_dust2`, 10 zBots, `-pingboost 4`, `sys_ticrate 1000`, pinned P-core
All three link the **same** `cs.so`, so only engine code differs.

---

## 1. Result: the harness cannot discriminate between compilers

### Frame execution p50, two runs each

| build | run a | run b |
|---|---|---|
| GCC | 18.1 µs | 18.7 µs |
| Clang | 9.2 µs | 5.1 µs |
| Clang + ThinLTO | 5.4 µs | **17.9 µs** |

Read naively this says Clang is 2–3× faster. It does not. **ThinLTO spans 5.4 → 17.9 µs
within its own condition**, covering the entire GCC range *and* the entire Clang range. The
distribution is bimodal, and both modes appear under the same build.

### Whole-window CPU, three runs each

Sampled over the full 23 s window rather than a percentile of the last 8 s, so it is far less
sensitive to which round phase the ring happened to capture:

| build | runs | range |
|---|---|---|
| GCC | 3.1, 3.3, 4.1 % | [3.1, 4.1] |
| Clang | 4.2, 2.7, 2.6 % | [2.6, 4.2] |
| Clang + ThinLTO | 3.7, 4.1 % | [3.7, 4.1] |

The ranges overlap almost completely. There is no separation to report.

Achieved rate was 998.2–999.1 Hz for every build, and edict counts were 270–271 every run —
so the entity workload was identical and the variation is bot *combat intensity*, which no
engine compiler flag affects.

## 2. Why this was predictable, and why it is the right answer

The measurement is not merely noisy; it is **structurally incapable** of resolving the
question:

- Engine code accounts for roughly **2 % of wall time** (`14-hot-path-profile.md`).
- Of the frame itself, `SV_Physics` dominates, and **97–99 % of its tail is
  `pfnStartFrame`** — GameDLL code (`18-physics-breakdown.md`).
- All three builds link the **identical** `cs.so`.

So even a hypothetical 30 % improvement in engine code would move total CPU by ~0.6
percentage points, against a run-to-run spread of ~1.5 points. The signal is an order of
magnitude below the noise floor **by construction**, not by bad luck.

Running more repeats would eventually resolve a difference this small, but it would be
resolving something that does not matter: at 2 % of wall time and ~2 % of a 1000 µs frame
budget, engine compiler choice is not what limits this server.

**This is the brief's own rule applied honestly** (§17.2: "never optimise an unmeasured
path", and do not report a performance claim without distribution evidence). The evidence
here says the path is not worth optimising, so the correct output is a negative result rather
than a number.

## 3. What *is* measurable and real

Binary size, which is deterministic:

| build | `engine_i486.so` | vs GCC |
|---|---|---|
| GCC `-O3` | 1,463,368 B | — |
| Clang `-O3` | 1,377,052 B | **−5.9 %** |
| Clang `-O3 -flto=thin` | 1,387,492 B | −5.2 % |

Interesting that ThinLTO is slightly *larger* than plain Clang here, presumably from
cross-TU inlining. Smaller text is weakly good for instruction-cache behaviour, but nothing
in the timing data lets me claim it translates into anything.

Also real, and the actually useful outcome of this workstream: **Clang now builds the whole
tree** (fixed in an earlier commit — all five HLTV sub-projects passed `-flto` at compile but
not at link, which GCC's linker plugin covered for). Clang is worth keeping working as a
*diagnostic* compiler — it is what would surface a second opinion on warnings, and it is the
gateway to sanitizers and to ARM64 later — rather than as a performance lever.

## 4. PGO was not attempted

The brief's matrix ends with ThinLTO + PGO. It was not run, deliberately: PGO's payoff is
better inlining and branch layout in hot code, and §2 establishes that the hot code is not in
the engine on this workload. Training a profile against bot AI would also bake in a workload
that `18-physics-breakdown.md` §3 shows is unrepresentative of a human server.

PGO becomes worth revisiting if either of these changes:
- Real-client benchmarking shows the engine's share of frame time is much higher than bots
  suggest (plausible — bots inflate `pfnStartFrame` and do not exercise lag compensation).
- A future workload makes engine code the bottleneck.

## 5. Recommendation

Keep **GCC** as the production compiler. There is no measured reason to switch, and it is
what CI and the release process already use.

Keep **Clang building** and use it for diagnostics, sanitizers and as the ARM64 path.

Revisit the matrix only with a deterministic workload — demo replay or scripted clients —
where a 0.6-point difference could actually be resolved. That is the same dependency the
hitbox work has.
