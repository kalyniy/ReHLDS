/*
* Frame scheduler instrumentation. See frameperf.h for rationale.
*/

#include "precompiled.h"

// Ring capacity. At sys_ticrate 2000 this holds ~4 s of admitted frames, which is enough
// to characterise p99.9 within a single dump without the buffer becoming cache-hostile.
#define FRAMEPERF_RING_SIZE 8192
#define FRAMEPERF_RING_MASK (FRAMEPERF_RING_SIZE - 1)

struct FramePerfSample
{
	uint64 wake_ns;              // loop woke from the pingboost sleep
	uint64 frame_start_ns;       // admitted frame began
	uint64 frame_end_ns;         // admitted frame finished
	uint64 intended_deadline_ns; // synthetic; see frameperf.h
	uint32 rejected_before;      // loop iterations rejected since the previous admitted frame
	uint32 flags;
};

cvar_t sv_rehlds_perf_frame = { "sv_rehlds_perf_frame", "0", 0, 0.0f, NULL };

static FramePerfSample g_ring[FRAMEPERF_RING_SIZE];
static uint64 g_written;          // total samples ever written; index = (g_written-1) & MASK
static uint64 g_epoch_ns;         // deadline epoch, set when sampling is enabled
static uint64 g_period_ns;        // 1e9 / sys_ticrate at enable time
static uint64 g_deadline_index;   // which deadline the next admitted frame is measured against
static uint32 g_rejected_run;     // rejected iterations since the last admitted frame
static uint64 g_wake_ns;          // wake time of the current iteration
static uint64 g_frame_start_ns;
static bool   g_enabled;
static uint64 g_total_admitted, g_total_rejected;

// Per-subsystem accumulation for the frame currently executing, flushed into the rings by
// FramePerf_OnFrameEnd. Stored in nanoseconds; uint32 caps at 4.3 s, far above any frame.
static uint64 g_subStart[FP_SUB_COUNT];
static uint32 g_subAccum[FP_SUB_COUNT];
static uint32 g_subRing[FP_SUB_COUNT][FRAMEPERF_RING_SIZE];

static const char *const g_subNames[FP_SUB_COUNT] = {
	"SV_ReadPackets",
	"SV_Physics",
	"SV_SendClientMsgs",
};

static uint64 FramePerf_Now()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64)ts.tv_sec * 1000000000ull + (uint64)ts.tv_nsec;
}

static void FramePerf_Reset()
{
	Q_memset(g_ring, 0, sizeof(g_ring));
	g_written = 0;
	g_rejected_run = 0;
	g_deadline_index = 0;
	g_total_admitted = 0;
	g_total_rejected = 0;
	g_epoch_ns = FramePerf_Now();

	float fps = sys_ticrate.value;
	if (fps < 1.0f)
		fps = 1.0f;
	g_period_ns = (uint64)(1000000000.0 / (double)fps);
	if (g_period_ns == 0)
		g_period_ns = 1;
}

void FramePerf_OnWake()
{
	bool want = (sv_rehlds_perf_frame.value != 0.0f);
	if (!want)
	{
		g_enabled = false;
		return;
	}

	if (!g_enabled)
	{
		g_enabled = true;
		FramePerf_Reset();
	}

	g_wake_ns = FramePerf_Now();
}

void FramePerf_OnAdmission(qboolean admitted)
{
	if (!g_enabled)
		return;

	if (!admitted)
	{
		g_rejected_run++;
		g_total_rejected++;
	}
}

void FramePerf_OnFrameBegin()
{
	if (!g_enabled)
		return;

	Q_memset(g_subAccum, 0, sizeof(g_subAccum));
	g_frame_start_ns = FramePerf_Now();
}

void FramePerf_SubBegin(int subsystem)
{
	if (!g_enabled || subsystem < 0 || subsystem >= FP_SUB_COUNT)
		return;

	g_subStart[subsystem] = FramePerf_Now();
}

void FramePerf_SubEnd(int subsystem)
{
	if (!g_enabled || subsystem < 0 || subsystem >= FP_SUB_COUNT)
		return;

	// A begin that happened before sampling was switched on leaves a zero start; ignore it
	// rather than accumulating a bogus multi-second interval.
	if (!g_subStart[subsystem])
		return;

	g_subAccum[subsystem] += (uint32)(FramePerf_Now() - g_subStart[subsystem]);
	g_subStart[subsystem] = 0;
}

void FramePerf_OnFrameEnd()
{
	if (!g_enabled)
		return;

	uint64 end = FramePerf_Now();

	FramePerfSample *s = &g_ring[g_written & FRAMEPERF_RING_MASK];
	s->wake_ns = g_wake_ns;
	s->frame_start_ns = g_frame_start_ns;
	s->frame_end_ns = end;

	// Prefer the deadline the scheduler actually released this frame at. Falling back to
	// the synthetic one would measure the phase difference between two independently
	// anchored deadlines, which is not lateness and is not meaningful.
	s->intended_deadline_ns = g_bSchedDeadlineActive
		? g_SchedFrameDeadlineNs
		: g_epoch_ns + g_deadline_index * g_period_ns;
	s->rejected_before = g_rejected_run;
	s->flags = 0;

	for (int i = 0; i < FP_SUB_COUNT; i++)
		g_subRing[i][g_written & FRAMEPERF_RING_MASK] = g_subAccum[i];

	g_written++;

	g_total_admitted++;
	g_rejected_run = 0;

	// Advance the synthetic deadline past every period this frame has already consumed, so
	// lateness is reported against the next deadline actually in the future rather than
	// accumulating an ever-growing backlog.
	do {
		g_deadline_index++;
	} while (g_epoch_ns + g_deadline_index * g_period_ns <= end);
}

static int FramePerf_CmpU64(const void *a, const void *b)
{
	uint64 x = *(const uint64 *)a;
	uint64 y = *(const uint64 *)b;
	return (x > y) - (x < y);
}

// Percentile of a sorted array, linearly interpolated, returned in microseconds.
static double FramePerf_Pct(const uint64 *sorted, int n, double p)
{
	if (n <= 0)
		return 0.0;

	double idx = p * (double)(n - 1);
	int lo = (int)idx;
	int hi = (lo + 1 < n) ? lo + 1 : lo;
	double f = idx - (double)lo;
	return ((double)sorted[lo] * (1.0 - f) + (double)sorted[hi] * f) / 1000.0;
}

static void FramePerf_Report(const char *label, uint64 *v, int n, const char *unit)
{
	if (n <= 0)
	{
		Con_Printf("  %-18s (no samples)\n", label);
		return;
	}

	qsort(v, n, sizeof(uint64), FramePerf_CmpU64);
	Con_Printf("  %-18s p50=%9.1f p95=%9.1f p99=%9.1f p99.9=%10.1f max=%11.1f %s\n",
		label,
		FramePerf_Pct(v, n, 0.50), FramePerf_Pct(v, n, 0.95), FramePerf_Pct(v, n, 0.99),
		FramePerf_Pct(v, n, 0.999), (double)v[n - 1] / 1000.0, unit);
}

static void FramePerf_Dump_f()
{
	if (!g_enabled || g_written == 0)
	{
		Con_Printf("frameperf: no samples. Set sv_rehlds_perf_frame 1 and let the server run.\n");
		return;
	}

	int n = (int)((g_written < FRAMEPERF_RING_SIZE) ? g_written : FRAMEPERF_RING_SIZE);
	uint64 first = g_written - n;

	// Scratch arrays are static rather than stack-allocated: 3 * 8192 * 8 = 192 KB would
	// overflow the default thread stack on some configurations.
	static uint64 interval[FRAMEPERF_RING_SIZE];
	static uint64 execution[FRAMEPERF_RING_SIZE];
	static uint64 lateness[FRAMEPERF_RING_SIZE];

	int ni = 0, ne = 0, nl = 0;
	uint64 overruns = 0, early = 0, rejected_in_window = 0;
	uint64 prev_start = 0;

	for (int i = 0; i < n; i++)
	{
		const FramePerfSample *s = &g_ring[(first + i) & FRAMEPERF_RING_MASK];

		if (i > 0 && s->frame_start_ns > prev_start)
			interval[ni++] = s->frame_start_ns - prev_start;
		prev_start = s->frame_start_ns;

		if (s->frame_end_ns > s->frame_start_ns)
			execution[ne++] = s->frame_end_ns - s->frame_start_ns;

		// Lateness is only meaningful when the frame started at or after its deadline.
		// Frames admitted early are counted separately rather than folded in as negatives.
		if (s->frame_start_ns >= s->intended_deadline_ns)
			lateness[nl++] = s->frame_start_ns - s->intended_deadline_ns;
		else
			early++;

		if (s->frame_end_ns > s->intended_deadline_ns + g_period_ns)
			overruns++;

		rejected_in_window += s->rejected_before;
	}

	double span = 0.0;
	{
		const FramePerfSample *a = &g_ring[first & FRAMEPERF_RING_MASK];
		const FramePerfSample *b = &g_ring[(g_written - 1) & FRAMEPERF_RING_MASK];
		if (b->frame_start_ns > a->frame_start_ns)
			span = (double)(b->frame_start_ns - a->frame_start_ns) / 1e9;
	}

	Con_Printf("--- frameperf: %d samples over %.2fs ---\n", n, span);
	Con_Printf("  sys_ticrate=%.0f  target_period=%.1f us  achieved=%.1f Hz\n",
		sys_ticrate.value, (double)g_period_ns / 1000.0,
		span > 0.0 ? (double)ni / span : 0.0);

	FramePerf_Report("frame interval", interval, ni, "us");
	FramePerf_Report("frame execution", execution, ne, "us");
	FramePerf_Report("deadline lateness", lateness, nl, "us");

	// Subsystem breakdown. Percentiles are per-frame totals, so the p50 column does not sum
	// to the frame-execution p50 -- different frames peak in different stages.
	{
		static uint64 scratch[FRAMEPERF_RING_SIZE];
		Con_Printf("  -- subsystem breakdown (per admitted frame) --\n");
		for (int sub = 0; sub < FP_SUB_COUNT; sub++)
		{
			uint64 total = 0;
			for (int i = 0; i < n; i++)
			{
				scratch[i] = (uint64)g_subRing[sub][(first + i) & FRAMEPERF_RING_MASK];
				total += scratch[i];
			}
			FramePerf_Report(g_subNames[sub], scratch, n, "us");
			Con_Printf("  %-18s share of measured frame time: %5.1f%%\n", "",
				span > 0.0 ? 100.0 * (double)total / (span * 1e9) : 0.0);
		}
	}

	Con_Printf("  admitted=%llu rejected=%llu reject_ratio=%.3f\n",
		(unsigned long long)g_total_admitted, (unsigned long long)g_total_rejected,
		(g_total_admitted + g_total_rejected)
			? 1.0 - (double)g_total_admitted / (double)(g_total_admitted + g_total_rejected)
			: 0.0);
	Con_Printf("  overruns=%llu (frames finishing past the next deadline)  admitted_early=%llu\n",
		(unsigned long long)overruns, (unsigned long long)early);
	Con_Printf("  rejected iterations per admitted frame (window avg)=%.2f\n",
		n ? (double)rejected_in_window / (double)n : 0.0);

	if (g_bSchedDeadlineActive)
		Con_Printf("  deadline source: -pingboost 4 scheduler (real deadlines), skipped=%llu\n",
			(unsigned long long)g_SchedMissedDeadlines);
	else
		Con_Printf("  NOTE: the deadline above is synthetic. This scheduler does not\n"
		           "        target one; see docs/audit/01-frame-scheduler.md.\n");
}

static void FramePerf_Reset_f()
{
	if (!g_enabled)
	{
		Con_Printf("frameperf: not enabled (sv_rehlds_perf_frame is 0).\n");
		return;
	}

	FramePerf_Reset();
	Con_Printf("frameperf: counters reset.\n");
}

void FramePerf_Init()
{
	Cvar_RegisterVariable(&sv_rehlds_perf_frame);
	Cmd_AddCommand("rehlds_perf_frame_dump", FramePerf_Dump_f);
	Cmd_AddCommand("rehlds_perf_frame_reset", FramePerf_Reset_f);
}
