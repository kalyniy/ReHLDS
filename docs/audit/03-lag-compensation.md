# Audit 03 — Lag Compensation and Historical Hitbox Coherence

**Baseline commit:** `0124d56` (tag `baseline-x86-32`) + ReGameDLL_CS `6799732` (reference only)
**Date:** 2026-08-15
**Method:** source inspection only. Every structural claim below was independently
re-verified against the working tree by a second reader before being recorded here.

> **Provenance.** This audit was derived entirely from the local source tree. Upstream
> issue #1190 and PR #1191 were **not** fetched, read, cloned, or used as input, in line
> with the owner's quarantine directive. `Garey27/hitbox_fixer` was **not** consulted for
> this document either — the `numblends == 1` behaviour noted in §5 was reached from
> ReHLDS/ReGameDLL source, not from that project. No third-party code is reproduced here.

---

## Verdict

**The rewind is origin-only. The entire pose is live.**

`SV_SetupMove` reconstructs exactly one historical quantity — the victim's `origin` —
and nothing else. Every other input to hitbox construction is read from the live edict at
trace time: `angles`, `sequence`, `frame`, `controller[]`, `blending[]`, the
`FL_DUCKING`-derived hull, and (under the blending interface ReGameDLL installs) the live
C++ members `m_flYaw`, `m_flPitch`, `m_iGaitsequence` and `m_flGaityaw`.

The result is a chimera: **the victim's skeleton is posed exactly as it is now, translated
to where they were `latency + interp` milliseconds ago.**

The historical data required to do this correctly is already stored and simply unused. The
`entity_state_t` records in the client frame history carry `angles`, `sequence`, `frame`,
`animtime`, `framerate`, `controller[4]`, `blending[4]` and `gaitsequence`. `SV_SetupMove`
reads only `origin`, `health` and `effects` from them.

This is **proven from source** as a statement about what the code does. The *magnitude* of
resulting hit/miss error is **not yet measured** and must be established in Phase 3 before
any fix is written.

---

## 1. What is actually saved and restored

The storage struct settles the question — it has no animation members at all
(`rehlds/engine/sv_user.h:38-50`):

```c
typedef struct sv_adjusted_positions_s
{
	int active;
	int needrelink;
	vec3_t neworg;
	vec3_t oldorg;
	vec3_t initial_correction_org;
	vec3_t oldabsmin;
	vec3_t oldabsmax;
	int deadflag;
	vec3_t temp_org;
	int temp_org_setflag;
} sv_adjusted_positions_t;
```

The rewind write is origin-only (`sv_user.cpp:1434-1441`):

```c
if (!VectorCompare(origin, cl->edict->v.origin))
{
    cl->edict->v.origin[0] = origin[0];
    cl->edict->v.origin[1] = origin[1];
    cl->edict->v.origin[2] = origin[2];
    SV_LinkEdict(cl->edict, FALSE);
    pos->needrelink = 1;
}
```

And the interpolation between the two bracketing snapshots is likewise origin-only
(`sv_user.cpp:1413-1424`) — `state` and `pnextstate` are full `entity_state_t` records,
but only `origin` is touched:

```c
pnextstate = SV_FindEntInPack(state->number, &frame->entities);
if (pnextstate)
{
    delta[0] = pnextstate->origin[0] - state->origin[0];
    delta[1] = pnextstate->origin[1] - state->origin[1];
    delta[2] = pnextstate->origin[2] - state->origin[2];
    VectorMA(state->origin, frac, delta, origin);
}
```

`oldabsmin`/`oldabsmax` are never written back; they exist only so `SV_GetTrueMinMax`
(`sv_user.cpp:1192-1208`) can hand *uncompensated* bounds to the player-movement path.
Actual `absmin`/`absmax` are recomputed as a side effect of `SV_LinkEdict`.

## 2. What the hitbox path reads instead

`SV_HullForStudioModel` (`rehlds/engine/r_studio.cpp:775-801`) pulls every argument from
the live edict; `v.origin` is the only rewound one:

```c
pstudiohdr = (studiohdr_t *)Mod_Extradata(g_psv.models[pEdict->v.modelindex]);
mstudioseqdesc_t* pseqdesc = (mstudioseqdesc_t *)((char*)pstudiohdr + pstudiohdr->seqindex);
pseqdesc += pEdict->v.sequence;                 // LIVE
angles[0..2] = pEdict->v.angles[0..2];          // LIVE
R_StudioPlayerBlend(pseqdesc, &iBlend, angles); // LIVE pitch -> blend
unsigned char blending[2]   = { (unsigned char)iBlend, 0 };
unsigned char controller[4] = { 0x7F, 0x7F, 0x7F, 0x7F };
return R_StudioHull(..., pEdict->v.frame,       // LIVE
                         pEdict->v.sequence,    // LIVE
                         angles, pEdict->v.origin,  // REWOUND
                         ...);
```

### 2.1 ReGameDLL replaces the bone setup, and reads four more live values

`sv_blending_interface_t` is overridden at DLL load
(`rehlds/engine/host_cmd.cpp:3313` resolves `Server_GetBlendingInterface`;
ReGameDLL supplies it at `regamedll/dlls/animation.cpp:527`). On a CS server the bone
setup that actually runs is ReGameDLL's, which reads live C++ members that are not
arguments at all and therefore cannot be rewound by any engine-side change:

```c
// ReGameDLL_CS/regamedll/dlls/animation.cpp:1054-1055
s = GetPlayerYaw(pEdict);      // -> pPlayer->m_flYaw
t = GetPlayerPitch(pEdict);    // -> pPlayer->m_flPitch

// :1147
int gaitsequence = GetPlayerGaitsequence(pEdict);

// :1183  -- OVERWRITES the yaw the engine passed in
temp_angles[1] = UTIL_GetPlayerGaitYaw(ENTINDEX(pEdict));
```

**The root bone matrix yaw comes from live `m_flGaityaw`.** This is the single most
severe item: a victim who turns during the compensation window has their hitboxes rotated
to their *current* facing at their *historical* position. `m_flGaityaw` is not networked
and has no representation in the frame history, so fixing this correctly requires
server-side history that does not exist today — an engine-only fix cannot reach it.

### 2.2 Coverage cross-check

| Bone-setup input | Rewound? | Present in `entity_state_t` history? |
|---|---|---|
| `origin` | **yes** | yes |
| `angles` (pitch/roll) | no | yes |
| `sequence` | no | yes |
| `frame` | no | yes |
| `controller[4]` | no (forced `0x7F`) | yes |
| `blending[2]` | no | yes |
| `size` / hull (`FL_DUCKING`) | no | yes (as `usehull`) |
| `modelindex` | no | yes |
| `m_flYaw`, `m_flPitch` | no | **not networked at all** |
| `m_iGaitsequence` | no | yes (as `gaitsequence`) |
| `m_flGaityaw` (root yaw) | no | **not networked at all** |
| `gamestate` (`bSkipShield`, changes hull count) | no | no |

The broadphase mixes epochs the same way: `SV_CheckSphereIntersection`
(`r_studio.cpp:845-875`) sizes its rejection sphere from the **live** sequence but centres
it on the **rewound** origin.

The duck row is the one case where the wrong pose is also the wrong *height* — the
crouch/stand distinction is baked into `pev->sequence` when `SetAnimation` runs
(`ReGameDLL_CS/regamedll/dlls/player.cpp:2647`), so a victim standing at `targettime` but
ducking at trace time gets crouch hitboxes anchored to the standing origin.

## 3. Jumping, reloading and dying victims are not compensated at all

> **MEASURED, AND LARGELY REFUTED — see `12-real-client-lagcomp.md`.**
>
> The mechanism below is real: the code path fires, and it was observed firing. But the
> predicted *magnitude* was wrong by three orders of magnitude. Over 46,328 victim
> evaluations from a real connected client, `EF_NOINTERP` accounted for **18 exclusions —
> 0.04 %**, not the dominant effect this section anticipated.
>
> This section called it "the most directly falsifiable claim in this document" and
> suggested it "may well dominate in practice". It does not. Measurement disagreed with
> source-derived reasoning, and measurement wins.
>
> The dominant exclusion is `health <= 0` at 9.46 %, which is correct behaviour — dead
> players should not be rewound. Overall **90.4 % of victim evaluations were compensated.**
>
> The original reasoning is preserved below because it is still a correct reading of the
> code; what it lacked was the observation that `SV_CleanupEnts` clears the flag every
> server frame, so it is set for roughly one frame in a thousand while snapshots are sampled
> at the update rate. The window for it to land in a stored snapshot is therefore tiny.

Independent of the pose problem, and arguably easier to observe. `SV_SetupMove` walks the
frame history and disqualifies a victim outright if **any** scanned frame carries
`EF_NOINTERP` (`sv_user.cpp:1341-1342`):

```c
if (state->effects & EF_NOINTERP)
    pos->deadflag = 1;
```

`deadflag` then skips that victim in the rewind loop (`sv_user.cpp:1404-1405`), so they are
traced at their **live** origin.

ReGameDLL raises `EF_NOINTERP` on ordinary gameplay actions — verified at all three sites:

| Site | Action |
|---|---|
| `player.cpp:2811` | jump / hop (`ANIM_HOP` / `ANIM_LEAP`, non-looping sequence) |
| `player.cpp:2861` | **reload** (non-looping sequence) |
| `player.cpp:4020` | death |

The flag is cleared once per frame by `SV_CleanupEnts` (`sv_main.cpp:4991-4998`, called
from `sv_main.cpp:5269`), so it is transient — but because `SV_SetupMove` scans *every*
frame back to `targettime`, a single flagged frame anywhere in that window disqualifies
the victim for the entire shot.

**Prediction to test:** shooting a jumping or reloading opponent behaves as if
`sv_unlag 0` — i.e. the shooter must lead the target by their full latency. This is the
most directly falsifiable claim in this document and should be the first thing measured.

## 4. Studio bone cache — key is wrong in both directions

`r_studiocache_t` (`r_studio.cpp:31-44`) keys on
`pModel, frame, sequence, angles, origin, size, controller[4], blending[2]`.

- **Omitted but load-bearing:** `m_flYaw`, `m_flPitch`, `m_iGaitsequence`, `m_flGaityaw`,
  and `bSkipShield` — the last of which changes `*pNumHulls` itself (`r_studio.cpp:658`)
  yet reaches neither the lookup nor the insert.
- **Included but inert:** `blending[2]` is in the key, but in the `numblends == 9` branch
  (the normal CS player case) `pblending` is never read — the interpolants come from
  `m_flYaw`/`m_flPitch`. Likewise `angles[1]` is in the key but is discarded at
  `animation.cpp:1183`.

Because the rewound `origin` *is* in the key, a rewind normally keys differently from the
live query and simply misses. The reachable failure is the converse — identical key,
different live gait state — which requires `pev->frame` to be pinned (a finished
non-looping sequence clamps it, `ReGameDLL_CS/regamedll/dlls/animating.cpp:33-40`) while
`m_flGaityaw` continues converging. Plausible; frequency unmeasured.

Invalidation is essentially absent: `R_FlushStudioCache` is called from exactly one place,
arena overflow inside `R_AddToStudioCache` (`r_studio.cpp:113-114`). Nothing flushes per
frame, per map change, or on model reload — `Mod_ClearAll` does not touch it — and
`pModel` is compared by raw pointer identity, so a recycled model address can alias a
pre-`changelevel` entry. Low probability, real hole.

`r_cachestudio` (default `1`) disables the cache when `0` and is the clean lever for
isolating cache effects during testing.

## 5. `numblends == 1` loses the blend entirely

In the engine's default bone setup the blend is applied only when `numblends > 1`
(`r_studio.cpp:562-575`); at `numblends == 1` the whole block is skipped and `pblending`
is silently discarded. Note also that only `blending[0]` is ever consumed. That collides
with the marshalling in `SV_HullForStudioModel`, which packs the **pitch** blend into
slot 0 (`r_studio.cpp:786-788`), whereas the game's convention is slot 0 = yaw, slot 1 =
pitch (`player.cpp:9081-9082`, `9138-9139`).

ReGameDLL's replacement has three regimes (`animation.cpp:1034-1046`): `numblends == 1` →
no blend at all; `2..8` or `>9` → two-way from `pblending[0]`; `== 9` → nine-way from live
`m_flYaw`/`m_flPitch`. The gait overlay is additionally gated on `numblends == 9`
(`animation.cpp:1144`).

Whether the shipped CS player models actually contain `numblends == 1` sequences is
**model-data dependent and unverified** — it requires enumerating `pseqdesc->numblends`
over the real `player/*.mdl` files. Do not assume this bug class is live here until that
enumeration is done.

## 6. Balance of save/restore

Fake clients are handled symmetrically — both call sites share the same
`if (!host_client->fakeclient)` guard (`sv_user.cpp:808-809`, `1081-1082`). Early guards in
`SV_SetupMove` are covered by the `nofind` flag, which `SV_RestoreMove` consumes first
(`sv_user.cpp:1450-1454`). All three `SV_RunCmd` early returns occur *before* the window
opens, and there is no `return` between `:809` and `:1082`.

Three structural leak paths remain, none triggered by stock ReGameDLL:

1. **Guard asymmetry.** `SV_RestoreMove` re-evaluates its own copies of `sv_unlag`, `lw`,
   `lc`, `active` and `pfnAllowLagCompensation` against **live** state
   (`sv_user.cpp:1456-1463`). Any of them changing inside the window — all reachable from
   plugin code running in `PreThink`/`PM_Move`/`PostThink`/`CmdEnd` — skips the restore
   and leaves every rewound player displaced. Since Metamod plugins routinely toggle
   `sv_unlag`, this is the highest-probability real-world leak.
2. **Non-local exit.** `Sys_Error`/`Host_Error` inside the trace longjmps past the restore.
   Two reachable sites on the exact path: `world.cpp:1181` and `world.cpp:241`.
3. **Re-entrancy.** `truepositions[]` and `nofind` are file-scope globals
   (`sv_user.cpp:31,42`) that `SV_SetupMove` unconditionally clears. `PF_RunPlayerMove_I`
   (`pr_cmds.cpp:2035-2069`) does not validate that its target is actually a fake client,
   so a plugin calling it on a real client mid-window would wipe the outer save array.

Non-player entities are never rewound — all loops are bounded by `g_psvs.maxclients`.

## 7. What this means for Phase 3

The defect is real and its mechanism is understood, but **no fix should be written yet.**
Order of work:

1. **Instrument first.** Log, per bullet: `targettime`, the two bracketing snapshot times
   and `frac`, the interpolated origin, the historical `angles`/`sequence`/`frame`/
   `blending`/`controller` from the same snapshots, the live values of all of those, the
   live `m_flGaityaw`/`m_flYaw`/`m_flPitch`/`m_iGaitsequence`, and the `TraceResult`.
   This makes the divergence directly observable rather than inferred.
2. **Measure magnitude** before deciding severity ranking. A yaw error only matters in
   proportion to the victim's angular velocity times the window; the `EF_NOINTERP`
   exclusion (§3) may well dominate in practice and is much cheaper to fix.
3. **Cheap triage lever:** `sv_clienttrace 0` collapses players to bbox tracing and
   bypasses bone setup and the cache entirely. If complaints vanish under
   `sv_clienttrace 0` but persist under `sv_clienttrace 1` with `r_cachestudio 0`, the
   fault is in bone reconstruction rather than the cache.
4. **Scope constraint to respect.** `m_flGaityaw`/`m_flYaw`/`m_flPitch` live in ReGameDLL
   and are not networked. A complete fix therefore spans both repositories and needs new
   server-side per-frame history. That is a design decision with ABI implications and
   should be planned, not improvised. An **owner-controlled ReGameDLL fork does not yet
   exist** and must be created before any ReGameDLL change is committed.
5. The engine-side subset (`sequence`, `frame`, `angles`, `blending`, `controller`,
   `usehull`) *is* already in the frame history and could be rewound without touching
   ReGameDLL — but doing so while `m_flGaityaw` stays live may produce a *differently*
   wrong pose rather than a correct one. Partial fixes here need differential evidence,
   not intuition.
