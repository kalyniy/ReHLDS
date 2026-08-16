/*
* Hit-registration instrumentation. See hitreg_stats.h for rationale.
*/

#include "precompiled.h"

cvar_t sv_rehlds_perf_hitreg = { "sv_rehlds_perf_hitreg", "0", 0, 0.0f, NULL };

static uint64 g_lagcomp[HITREG_LC_COUNT];
static uint64 g_studioCacheHit, g_studioCacheMiss;
static uint64 g_hitgroup[16];
static uint64 g_playerHits;
static bool   g_enabled;

static const char *const g_lagcompNames[HITREG_LC_COUNT] = {
	"rewound",
	"unchanged",
	"skip: dead (health<=0)",
	"skip: EF_NOINTERP (jump/reload/death)",
	"skip: teleport detected",
	"skip: absent from scanned frames",
};

// MAX_HITGROUPS is a gamedll concept; the engine only ever forwards the value. These are
// the standard Half-Life/Counter-Strike groups, used for labelling only.
static const char *const g_hitgroupNames[16] = {
	"generic", "head", "chest", "stomach", "left arm", "right arm",
	"left leg", "right leg", "8", "9", "10", "11", "12", "13", "14", "15",
};

static inline bool HitReg_Enabled()
{
	// Read the cvar rather than caching it, so the operator can toggle mid-run. This is a
	// float compare on a hot-ish path; the surrounding work (bone setup, trace) dwarfs it.
	return sv_rehlds_perf_hitreg.value != 0.0f;
}

void HitReg_LagCompOutcome(int outcome)
{
	if (!HitReg_Enabled())
		return;

	if (outcome < 0 || outcome >= HITREG_LC_COUNT)
		return;

	g_enabled = true;
	g_lagcomp[outcome]++;
}

void HitReg_StudioHull(qboolean cacheHit)
{
	if (!HitReg_Enabled())
		return;

	g_enabled = true;
	if (cacheHit)
		g_studioCacheHit++;
	else
		g_studioCacheMiss++;
}

void HitReg_PlayerHit(int hitgroup)
{
	if (!HitReg_Enabled())
		return;

	g_enabled = true;
	g_playerHits++;
	if (hitgroup >= 0 && hitgroup < 16)
		g_hitgroup[hitgroup]++;
}

// --- pose divergence sampling -------------------------------------------------------
// A rewound victim is placed at a historical origin but posed from live state. These
// samples measure the gap between the two epochs.

#define POSE_RING_SIZE 8192
#define POSE_RING_MASK (POSE_RING_SIZE - 1)

struct PoseSample
{
	float yawDelta;      // |historical yaw - live yaw|, wrapped to [0,180]
	float pitchDelta;    // likewise for pitch
	float rewindMs;      // how far back the origin was moved
	uint8 seqMismatch;   // historical sequence  != live sequence
	uint8 gaitMismatch;  // historical gaitsequence != live gaitsequence
};

static PoseSample g_pose[POSE_RING_SIZE];
static uint64 g_poseWritten;
static uint64 g_poseSeqMismatch, g_poseGaitMismatch;

// Shortest angular distance between two Euler angles, in [0,180].
static float HitReg_AngleDelta(float a, float b)
{
	float d = a - b;
	while (d > 180.0f) d -= 360.0f;
	while (d < -180.0f) d += 360.0f;
	return d < 0.0f ? -d : d;
}

void HitReg_PoseDivergence(const struct entity_state_s *hist, const struct edict_s *live, float rewindSecs)
{
	if (!HitReg_Enabled() || !hist || !live)
		return;

	g_enabled = true;

	PoseSample *s = &g_pose[g_poseWritten & POSE_RING_MASK];
	s->pitchDelta = HitReg_AngleDelta(hist->angles[0], live->v.angles[0]);
	s->yawDelta   = HitReg_AngleDelta(hist->angles[1], live->v.angles[1]);
	s->rewindMs   = rewindSecs * 1000.0f;
	s->seqMismatch  = (hist->sequence != live->v.sequence) ? 1 : 0;
	s->gaitMismatch = (hist->gaitsequence != live->v.gaitsequence) ? 1 : 0;

	if (s->seqMismatch)  g_poseSeqMismatch++;
	if (s->gaitMismatch) g_poseGaitMismatch++;

	g_poseWritten++;
}

static void HitReg_Reset()
{
	Q_memset(g_lagcomp, 0, sizeof(g_lagcomp));
	Q_memset(g_hitgroup, 0, sizeof(g_hitgroup));
	Q_memset(g_pose, 0, sizeof(g_pose));
	g_studioCacheHit = g_studioCacheMiss = 0;
	g_playerHits = 0;
	g_poseWritten = 0;
	g_poseSeqMismatch = g_poseGaitMismatch = 0;
}

static int HitReg_CmpFloat(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

static double HitReg_PctF(const float *sorted, int n, double p)
{
	if (n <= 0)
		return 0.0;
	double idx = p * (double)(n - 1);
	int lo = (int)idx;
	int hi = (lo + 1 < n) ? lo + 1 : lo;
	double f = idx - (double)lo;
	return (double)sorted[lo] * (1.0 - f) + (double)sorted[hi] * f;
}

static void HitReg_ReportPose(const char *label, float *v, int n, const char *unit)
{
	qsort(v, n, sizeof(float), HitReg_CmpFloat);
	Con_Printf("  %-14s p50=%8.2f p90=%8.2f p99=%8.2f p99.9=%8.2f max=%9.2f %s\n",
		label,
		HitReg_PctF(v, n, 0.50), HitReg_PctF(v, n, 0.90), HitReg_PctF(v, n, 0.99),
		HitReg_PctF(v, n, 0.999), (double)v[n - 1], unit);
}

static void HitReg_Dump_f()
{
	if (!g_enabled)
	{
		Con_Printf("hitreg: no samples. Set sv_rehlds_perf_hitreg 1 and play some rounds.\n");
		return;
	}

	uint64 lcTotal = 0;
	for (int i = 0; i < HITREG_LC_COUNT; i++)
		lcTotal += g_lagcomp[i];

	Con_Printf("--- hitreg: lag compensation outcomes (%llu victim-evaluations) ---\n",
		(unsigned long long)lcTotal);

	for (int i = 0; i < HITREG_LC_COUNT; i++)
	{
		Con_Printf("  %-38s %10llu  %6.2f%%\n",
			g_lagcompNames[i], (unsigned long long)g_lagcomp[i],
			lcTotal ? 100.0 * (double)g_lagcomp[i] / (double)lcTotal : 0.0);
	}

	uint64 skipped = 0;
	for (int i = HITREG_LC_SKIP_DEAD; i < HITREG_LC_COUNT; i++)
		skipped += g_lagcomp[i];

	Con_Printf("  --> %.2f%% of victim-evaluations were NOT lag compensated\n",
		lcTotal ? 100.0 * (double)skipped / (double)lcTotal : 0.0);

	uint64 studioTotal = g_studioCacheHit + g_studioCacheMiss;
	Con_Printf("--- hitreg: studio hull construction (%llu) ---\n",
		(unsigned long long)studioTotal);
	Con_Printf("  cache hit=%llu miss=%llu  hit_ratio=%.2f%%\n",
		(unsigned long long)g_studioCacheHit, (unsigned long long)g_studioCacheMiss,
		studioTotal ? 100.0 * (double)g_studioCacheHit / (double)studioTotal : 0.0);

	// Pose divergence: how far the live pose has moved from the historical snapshot the
	// rewound origin came from. Non-zero values here ARE the defect in doc 03 -- the
	// hitbox is built from the right position and the wrong pose.
	int pn = (int)((g_poseWritten < POSE_RING_SIZE) ? g_poseWritten : POSE_RING_SIZE);
	if (pn > 0)
	{
		static float yaw[POSE_RING_SIZE], pitch[POSE_RING_SIZE], rew[POSE_RING_SIZE];
		uint64 first = g_poseWritten - pn;
		for (int i = 0; i < pn; i++)
		{
			const PoseSample *s = &g_pose[(first + i) & POSE_RING_MASK];
			yaw[i] = s->yawDelta;
			pitch[i] = s->pitchDelta;
			rew[i] = s->rewindMs;
		}

		Con_Printf("--- hitreg: rewound-pose divergence (%d samples of %llu) ---\n",
			pn, (unsigned long long)g_poseWritten);
		Con_Printf("  historical snapshot vs LIVE edict at hitbox construction time\n");
		HitReg_ReportPose("yaw delta", yaw, pn, "deg");
		HitReg_ReportPose("pitch delta", pitch, pn, "deg");
		HitReg_ReportPose("rewind", rew, pn, "ms");
		Con_Printf("  sequence mismatch=%llu (%.2f%%)  gaitsequence mismatch=%llu (%.2f%%)\n",
			(unsigned long long)g_poseSeqMismatch,
			100.0 * (double)g_poseSeqMismatch / (double)g_poseWritten,
			(unsigned long long)g_poseGaitMismatch,
			100.0 * (double)g_poseGaitMismatch / (double)g_poseWritten);
		Con_Printf("  NOTE: m_flGaityaw supplies the ROOT bone yaw and is neither networked\n");
		Con_Printf("        nor snapshotted, so its divergence cannot be measured here.\n");
	}

	Con_Printf("--- hitreg: player hits by hitgroup (%llu) ---\n",
		(unsigned long long)g_playerHits);
	for (int i = 0; i < 16; i++)
	{
		if (!g_hitgroup[i])
			continue;
		Con_Printf("  %-12s %10llu  %6.2f%%\n",
			g_hitgroupNames[i], (unsigned long long)g_hitgroup[i],
			g_playerHits ? 100.0 * (double)g_hitgroup[i] / (double)g_playerHits : 0.0);
	}
}

static void HitReg_Reset_f()
{
	HitReg_Reset();
	Con_Printf("hitreg: counters reset.\n");
}

void HitReg_Init()
{
	Cvar_RegisterVariable(&sv_rehlds_perf_hitreg);
	Cmd_AddCommand("rehlds_perf_hitreg_dump", HitReg_Dump_f);
	Cmd_AddCommand("rehlds_perf_hitreg_reset", HitReg_Reset_f);
}
