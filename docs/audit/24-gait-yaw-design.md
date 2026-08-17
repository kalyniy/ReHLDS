# Design 24 — Completing the Hitbox Fix: Rewinding Gait Yaw

**Date:** 2026-08-16
**Status:** **implemented**, behind `sv_rehlds_unlag_pose` (default `0`). Spans both repos:
ReHLDS `audit/m0-baseline` and ReGameDLL `feature/gaityaw-lagcomp`. Not yet validated against a
real client — see §5.

This is the missing half of `docs/audit/21`. That change rewinds the victim's animation
(sequence, frame, pitch) but explicitly not their gait yaw. This document establishes that the
remaining piece is tractable, costs nothing on the wire, and states exactly what it needs.

---

## 1. Why the current fix does not reach the hitboxes' orientation

ReGameDLL overrides the root rotation matrix unconditionally for players
(`regamedll/dlls/animation.cpp:1181-1187`):

```c
if (pEdict && CBaseEntity::Instance(...)->IsPlayer())
{
    temp_angles[1] = UTIL_GetPlayerGaitYaw(ENTINDEX(pEdict));
    if (temp_angles[1] < 0) temp_angles[1] += 360.0f;
}
AngleMatrix(temp_angles, (*g_pRotationMatrix));
```

`temp_angles` starts as a copy of the `angles` passed in, but index 1 — the yaw — is
**replaced** before the matrix is built. So the engine-side `ent->v.angles[1]` rewind added in
`docs/audit/21` does not affect the model's root orientation at all. It still corrects pitch
(which feeds `R_StudioPlayerBlend`) and the animation choice, but the direction the body faces
— which is what decides where the limb hitboxes land — comes from
`UTIL_GetPlayerGaitYaw` → `pPlayer->m_flGaityaw` (`util.cpp:1632-1637`), read **live**.

## 2. Why it cannot be recomputed instead

`m_flGaityaw` is not a function of the current snapshot. `CBasePlayer::StudioProcessGait`
(`player.cpp:8999-9046`) evolves it as a **stateful filter**:

```c
real_t flYawDiff = pev->angles.y - m_flGaityaw;      // carries previous value forward
...
m_flGaityaw += flYawDiff;                            // incremental
m_flGaityaw -= int64(m_flGaityaw / 360) * 360;
```

with additional hysteresis and ±180 flips at `player.cpp:9104-9122`. Given only a historical
origin, angle and velocity, the value is not reconstructible — the filter's own prior state is
an input. **It must be recorded, not derived.**

## 3. The mechanism: snapshot history already has room, at zero protocol cost

The important structural fact — and the one that makes this cheap — is that **lag compensation
reads the server's own snapshot history, not network data.** `SV_SetupMove` walks
`_host_client->frames[...].entities.entities[]`, which is the server's record of what it *sent*
to that client. Those are full `entity_state_t` structs, populated by the GameDLL in
`AddToFullPack`.

`delta.lst` governs only which fields are *encoded onto the wire*. A field the GameDLL writes
into `entity_state_t` is captured in frame history and readable by lag compensation **whether or
not it is ever transmitted**.

Surveying what is actually free:

| field | status |
|---|---|
| `pev->fuser1` | **taken** — CS stamina (`CBasePlayer::ResetStamina`), and forwarded to the owning client as `cd->fuser1` (`client.cpp:5078`) |
| `state->fuser1..4`, `state->vuser1..4`, `state->iuser1..4` | **free** — `AddToFullPack` never assigns them for players |
| `entity_state_player_t` delta | does **not** list any `fuser`/`vuser`/`iuser` — the `fuser1` at `delta.lst:63` is in `clientdata_t`, a different structure |

Note the distinction that makes this work: `pev->fuser1` (entvars, stamina) and
`state->fuser1` (snapshot) are **different fields**. Writing the latter does not disturb the
former, and adds nothing to Protocol 48's wire format.

## 4. The change, in three parts

1. **ReGameDLL — capture.** In `AddToFullPack`, for players:
   `state->fuser1 = ((CBasePlayer *)pPlayer)->m_flGaityaw;`
   One assignment. No wire impact, since the player delta does not encode `fuser1`.

2. **Engine — rewind.** In `SV_ApplyHistoricalPose`, interpolate `from->fuser1` toward
   `to->fuser1` with the existing `frac`, using **angular** interpolation — `SV_LerpAngle`, not
   a linear blend, because gait yaw is cyclic and a bracket straddling ±180 would otherwise
   sweep the body the long way round. Publish the result where the GameDLL can see it, and
   save/restore it under the same `applied*` guard as the rest of the pose.

3. **ReGameDLL — consume.** `UTIL_GetPlayerGaitYaw` returns the rewound value while a rewind is
   in effect, and `m_flGaityaw` otherwise. Outside the lag-comp window the two are equal, so
   normal play is bit-identical.

The engine→GameDLL channel in step 2/3 is the only genuinely new interface. It must not be a
new engine export, since that would break the ABI for existing mods; writing into an unused
`entvars_t` slot that ReGameDLL agrees to read is the compatible option.

## 5. What was built, and what remains unvalidated

Implemented exactly as designed above, using `fuser4` rather than `fuser1` — `pev->fuser1` is CS
stamina (`ResetStamina`, forwarded to the owning client as `cd->fuser1`), whereas `fuser4` is
unused by ReGameDLL, unused by `pm_shared`, and **absent from `delta.lst` entirely**, so it is
never transmitted for any entity type.

The design also collapsed to something simpler than §4 proposed: no "rewind active" flag is
needed. ReGameDLL *publishes* live `m_flGaityaw` into `pev->fuser4` every PostThink and always
reads back from `pev->fuser4`, so outside a rewind the two are identical and the engine only has
to save, overwrite and restore one field.

Confirmed while building it: `AddToFullPack` writes **directly into `frame->entities`** under
`REHLDS_OPT_PEDANTIC` (`sv_main.cpp:4889`), which is unconditionally defined — and the
non-pedantic path does a full `Q_memcpy` of `entity_state_t`. So the snapshot field survives
into the history `SV_SetupMove` reads under either build.

Gait yaw is interpolated with `SV_LerpAngle`, not linearly: it is cyclic, and a bracket
straddling ±180 would otherwise swing the body the long way round — the same defect
`SV_LerpFrame` exists to avoid for studio frames. The restore is guarded **separately** from the
rest of the pose, because game code can rewrite sequence/frame during the window (the
frozen-corpse case in `docs/audit/21`) while gait yaw moves independently; tying them together
would let one stand-down strand the other.

**What is verified.** The publish/read path is functionally proven: if the mirror were broken,
`UTIL_GetPlayerGaitYaw` would return 0 for every player, every model's root would face yaw 0,
and bot-vs-bot hit detection would collapse. Four interleaved 150 s 10-bot matches — baseline
16 and 18 frags, this build 18 and 20. Both engine and mod build clean, 35 tests pass, and the
server runs.

**What is not verified, and cannot be here.** The *rewind itself* has never executed, because
bots never enter `SV_SetupMove` — `SV_RunCmd` gates it on `!host_client->fakeclient`. So the
interpolation, the `applied*` guard and the restore are all reasoned and reviewed but unexercised.
The `gaityaw delta` figures in `rehlds_perf_hitreg_dump` will read zero until a human connects.

**The measurement to take first**, with `sv_rehlds_unlag_pose 0` — this quantifies the defect
without changing any behaviour, because `HitReg_PoseDivergence` samples before the pose is
touched:

```
sv_rehlds_perf_hitreg 1
<play>
rehlds_perf_hitreg_dump
```

Compare the new `gaityaw delta` percentiles against `yaw delta`. If gait divergence is small,
this whole change is not worth enabling; if it is comparable to or larger than view-yaw
divergence, it is the dominant hitbox error. Only then set `sv_rehlds_unlag_pose 1` and confirm
the gait figures fall.

## 5b. Original rationale for deferring

**It cannot be validated without a real client.** Bots never trigger any of this —
`SV_RunCmd` gates `SV_SetupMove` on `!host_client->fakeclient`, so every fake client skips
lag compensation entirely. The instrumentation that would show whether gait divergence actually
fell (`HitReg_PoseDivergence`, `docs/audit/22` §1) needs a human moving and shooting.

Shipping a gameplay-affecting change to hit registration that has never been exercised is
exactly the failure mode that produced the frozen-corpse regression in `docs/audit/21` — a
defect no amount of code review found and one playtest surfaced immediately. The correct
sequence is: implement behind the existing `sv_rehlds_unlag_pose` cvar (default `0`), then
measure yaw divergence before and after with a real client.

## 6. Expected effect, and the honest uncertainty

Gait yaw is the dominant term for limb hitboxes: the arms and legs swing furthest from the
model's origin, so a body rotated wrongly displaces them most. `docs/audit/22` measured yaw
divergence at p50 0.92° / p90 10.39° / p99 20.91° **after** the config fix — but that
instrument samples `pev->angles[1]`, the *view* yaw, not `m_flGaityaw`. Gait yaw lags view yaw
by design, so those numbers bound the problem only loosely and could understate or overstate it.

**Measure gait divergence directly before assuming this is worth shipping.** The cheap first
step is to extend `HitReg_PoseDivergence` to sample the new `state->fuser1` against live
`m_flGaityaw` — that quantifies the defect with only step 1 implemented, before any behaviour
changes at all.
