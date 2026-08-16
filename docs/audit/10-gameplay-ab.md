# Measurement 10 — Gameplay A/B for the Deadline Scheduler, and Its Limits

**Date:** 2026-08-16
**Harness:** `docs/audit/tools/gameplay_ab.sh`
**Setup:** `de_dust2`, 10 zBots, `bot_difficulty 2`, `sys_ticrate 1000`, 45 s warmup,
120 s sample. **Each condition run twice**, so within-condition variance can be compared
against the between-condition difference.

---

## 1. Results

| run | achieved Hz | player hits | studio hulls | cache hit ratio |
|---|---|---|---|---|
| `pb0_a` (default) | 906.7 | 428 | 2424 | 4.87 % |
| `pb0_b` (default) | 916.7 | 411 | 1807 | 13.72 % |
| `pb4_a` (`-pingboost 4`) | 997.0 | 333 | 1826 | 16.21 % |
| `pb4_b` (`-pingboost 4`) | 997.6 | 513 | 2232 | 10.62 % |

Hitgroup counts:

| run | head | chest | stomach | L arm | R arm | L leg | R leg |
|---|---|---|---|---|---|---|---|
| `pb0_a` | 38 | 69 | 67 | 104 | 50 | 50 | 50 |
| `pb0_b` | 28 | 85 | 52 | 65 | 62 | 30 | 89 |
| `pb4_a` | 21 | 53 | 53 | 69 | 46 | 53 | 38 |
| `pb4_b` | 43 | 47 | 68 | 113 | 180 | 23 | 39 |

## 2. The only signal that clears the noise floor is the frame rate

**Achieved Hz** separates cleanly: 906.7 / 916.7 for the default against 997.0 / 997.6 for
`-pingboost 4`. Within-condition spread is ~10 Hz and 0.6 Hz; between-condition gap is ~85 Hz.
That is a real effect, and it re-confirms `08-load-benchmark.md` on an independent set of runs.

**Every gameplay metric is dominated by run-to-run variance:**

- Player hits: `pb4` gave 333 and 513 — a 54 % spread **within one condition**.
- Cache hit ratio: `pb0` gave 4.87 % and 13.72 % — a 2.8× spread within one condition.
- Hitgroups: `pb4_b` recorded 180 right-arm hits against `pb4_a`'s 46, same build, same
  settings.

**Conclusion: this test design has enough power to detect a gross gameplay regression —
a crash, a weapon that stops registering, rounds that stop completing — and no more.** It
cannot detect subtle changes in hit registration, movement or spread. It should not be
presented as evidence that `-pingboost 4` is gameplay-neutral. It is evidence only that
nothing is grossly broken.

## 3. A finding retracted

`09-hitreg-instrumentation.md` §3 flagged an apparent hitbox asymmetry from a single run:
left leg 116 hits (32.77 %) against right leg 30 (8.47 %), a 3.9× ratio.

The repeat runs refute it. The left/right leg ratio across four runs is **1.0, 0.34, 1.4,
0.6** — it flips direction. There is no systematic bias; the original observation was noise
in a 354-hit sample.

The doc has been updated with a retraction rather than a silent edit. The methodological
point is the durable one:

> **A single bot run is not evidence for any gameplay statistic.** Bot behaviour varies
> enough to manufacture a 3.9× effect out of nothing. Every such measurement needs a
> within-condition repeat before it is interpreted, and `gameplay_ab.sh` runs each condition
> twice for exactly this reason.

Had the A/B been designed as one run of A against one run of B — the obvious design — it
would have produced a confident, entirely fabricated finding about hitbox asymmetry.

## 4. What is still not tested

The gameplay differential the brief requires before a timing change ships is **still not
done**. This run does not provide it, for two independent reasons:

1. **Insufficient power** (§2) — bot variance swamps everything but frame rate.
2. **Lag compensation never executes** (`09-hitreg-instrumentation.md` §1) — every bot is a
   fake client, and `SV_RunCmd` gates `SV_SetupMove` on `!host_client->fakeclient`. The
   entire lag-comp and historical-hitbox path is untouched by a bot-only server.

A real differential test needs deterministic inputs, which means either the upstream demo
replay suite (`rehldsorg/testdemos`, blocked: Docker is inactive and the user is not in the
`docker` group — an owner action) or a synthetic Protocol 48 client. The latter is the
better investment because it also unblocks the Phase 3 hitbox matrix.

**Status of `-pingboost 4`: measured, self-consistent, and not yet validated for gameplay.
Not production advice.**
