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

static void HitReg_Reset()
{
	Q_memset(g_lagcomp, 0, sizeof(g_lagcomp));
	Q_memset(g_hitgroup, 0, sizeof(g_hitgroup));
	g_studioCacheHit = g_studioCacheMiss = 0;
	g_playerHits = 0;
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
