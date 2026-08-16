/*
* Hit-registration instrumentation for ReHLDS.
*
* Two purposes, both from the project brief:
*
* 1. Shot-path telemetry (brief section 6.3) -- make a hit or miss explainable from server
*    state rather than from feel.
*
* 2. A gameplay-sensitive differential signal. Frame-timing changes such as the
*    -pingboost 4 scheduler cannot be validated by frame statistics alone; what matters is
*    whether they perturb gameplay. Lag-compensation outcomes and hitgroup distribution
*    over a long run are far more sensitive to that than round scores are.
*
* It also directly tests the claim in docs/audit/03-lag-compensation.md that a victim
* carrying EF_NOINTERP in any scanned frame is dropped from lag compensation entirely --
* and that ReGameDLL raises that flag on jump, reload and death. The per-reason counters
* below turn that from a source-derived inference into a measurement.
*
* Bounded counters only; no per-event allocation or formatting. Disabled by default.
*/

#pragma once

#include "maintypes.h"

enum HitRegLagCompOutcome
{
	HITREG_LC_REWOUND = 0,	// victim's origin was moved back in time
	HITREG_LC_UNCHANGED,	// considered, but historical origin equalled the live one
	HITREG_LC_SKIP_DEAD,	// state->health <= 0
	HITREG_LC_SKIP_NOINTERP,// state->effects & EF_NOINTERP  (jump / reload / death)
	HITREG_LC_SKIP_TELEPORT,// SV_UnlagCheckTeleport tripped
	HITREG_LC_SKIP_NOT_FOUND,// never seen in the scanned frames (out of PVS, evicted)
	HITREG_LC_COUNT
};

void HitReg_Init();

// Called once per victim per shooter command, with the reason that victim was or was not
// rewound. Cheap: a bounds-checked increment.
void HitReg_LagCompOutcome(int outcome);

// Called for every studio-hull construction, distinguishing cache hits from recomputes.
void HitReg_StudioHull(qboolean cacheHit);

// Called at the moment a victim's origin is rewound, with the historical snapshot the
// origin came from and the live edict whose pose the hitbox will actually be built from.
//
// This is the direct measurement of the chimera described in docs/audit/03-lag-compensation.md:
// SV_SetupMove restores origin and nothing else, so the studio hull is constructed from the
// victim's CURRENT angles/sequence/frame while standing at their PAST position. Sampling
// both sides here quantifies how far apart those two epochs actually are in practice,
// without needing to intercept individual bullets.
//
// rewindSecs is realtime - targettime, i.e. how far back the origin was moved.
void HitReg_PoseDivergence(const struct entity_state_s *hist, const struct edict_s *live, float rewindSecs);

// Called for every traceline that resolved against a player. NOT restricted to weapon fire:
// the bullet/knife/flash distinction lives in GameDLL-private trace_flags bits, which the
// engine deliberately does not interpret.
void HitReg_PlayerHit(int hitgroup);
