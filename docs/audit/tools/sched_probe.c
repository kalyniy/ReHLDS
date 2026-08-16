/*
 * sched_probe — faithful standalone replica of the ReHLDS dedicated-server frame loop.
 *
 * Purpose: measure the achieved frame-interval distribution of each -pingboost mode at a
 * given sys_ticrate, WITHOUT needing game content, so that Findings 2.1 and 3.1 in
 * docs/audit/01-frame-scheduler.md can be confirmed or refuted before any code is changed.
 *
 * What it replicates, and where each piece comes from in the baseline tree (0124d56):
 *
 *   rehlds/dedicated/src/sys_ded.cpp:156-177   the while(!bDone) { sys->Sleep(1); RunFrame(); } loop
 *   rehlds/dedicated/src/sys_linux.cpp:78-146  Sleep_Old / Sleep_Timer / Sleep_Select / Sleep_Net
 *   rehlds/engine/net_ws.cpp:979-1045          NET_Sleep_Timeout, including the
 *                                              (1000/fps)*1000 truncation and the
 *                                              numFrames/staggerFrames socket-wait stagger
 *   rehlds/engine/sys_engine.cpp:107-119       m_fFrameTime = Sys_FloatTime() - m_fOldTime
 *   rehlds/engine/host.cpp:663-727             Host_FilterTime admission gate, incl. the
 *                                              1.0f/(fps+1.0f) early-admission bias
 *   rehlds/engine/sys_dll.cpp:600-613          Sys_FloatTime on CLOCK_MONOTONIC
 *
 * It also implements two candidate replacements (absolute-deadline modes) so Phase 2 has a
 * measured comparison rather than an assumed one.
 *
 * This is a measurement instrument, not engine code. It links nothing from ReHLDS.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <netinet/in.h>

/* ---- Sys_FloatTime, matching engine/sys_dll.cpp:600-613 (CLOCK_MONOTONIC) ---------- */
static double Sys_FloatTime(void)
{
	static struct timespec start_time;
	static int bInitialized = 0;
	struct timespec now;

	if (!bInitialized) {
		bInitialized = 1;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	/* NB: the original omits start_time.tv_nsec here; reproduced faithfully. */
	return (now.tv_sec - start_time.tv_sec) + now.tv_nsec * 0.000000001;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- shared state ------------------------------------------------------------------ */
static double sys_ticrate  = 1000.0;
static double sys_timescale = 1.0;
static double realtime, oldrealtime;
static double host_frametime;
static int    g_sock = -1;

/* counters for NET_Sleep_Timeout branch accounting (Finding 3.2) */
static long g_net_socketwait, g_net_blindsleep;

/* ---- pingboost sleep primitives ---------------------------------------------------- */

/* sys_linux.cpp:78-81 */
static void Sleep_Old(int msec) { usleep(msec * 1000); }

/* sys_linux.cpp:83-92 */
static void Sleep_Select(int msec)
{
	struct timeval tv;
	tv.tv_sec  = 0;
	tv.tv_usec = 1000 * msec;
	select(1, NULL, NULL, NULL, &tv);
}

/* sys_linux.cpp:110-146 */
static volatile int g_bPaused = 0;
static void alarmFunc(int num)
{
	(void)num;
	signal(SIGALRM, alarmFunc);
	if (!g_bPaused) {
		struct itimerval itim;
		itim.it_interval.tv_sec = 0; itim.it_interval.tv_usec = 0;
		itim.it_value.tv_sec = 0;    itim.it_value.tv_usec = 1000;
		setitimer(ITIMER_REAL, &itim, 0);
	}
}
static void Sleep_Timer(int msec)
{
	struct itimerval tm;
	tm.it_value.tv_sec     = msec / 1000;
	tm.it_value.tv_usec    = (msec % 1000) * 1000;
	tm.it_interval.tv_sec  = 0;
	tm.it_interval.tv_usec = 0;
	g_bPaused = 0;
	if (!setitimer(ITIMER_REAL, &tm, NULL))
		pause();
	g_bPaused = 1;
}

/* net_ws.cpp:979-1045 — reproduced including both defects */
static int NET_Sleep_Timeout(void)
{
	static int32_t lasttime;
	static int numFrames;
	static int staggerFrames;

	int fps = (int)sys_ticrate;
	int32_t curtime = (int)Sys_FloatTime();
	if (lasttime) {
		if (curtime - lasttime > 1) {
			lasttime = curtime;
			numFrames = fps;
			staggerFrames = fps / 100 + 1;
		}
	} else {
		lasttime = curtime;
	}

	fd_set fdset;
	FD_ZERO(&fdset);

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = (1000 / fps) * 1000;   /* <-- integer truncation, net_ws.cpp:1006 */
	if (tv.tv_usec <= 0)
		tv.tv_usec = 1;

	int res;
	if (numFrames > 0 && numFrames % staggerFrames) {
		FD_SET(g_sock, &fdset);
		res = select(g_sock + 1, &fdset, NULL, NULL, &tv);
		g_net_socketwait++;
	} else {
		res = select(0, NULL, NULL, NULL, &tv);
		g_net_blindsleep++;
	}
	--numFrames;
	return res;
}
static void Sleep_Net(int msec) { (void)msec; NET_Sleep_Timeout(); }

/* ---- candidate replacements (Phase 2 designs, not present in baseline) -------------- */

/* absolute monotonic deadline via clock_nanosleep(TIMER_ABSTIME) */
static uint64_t g_deadline_ns;
static uint64_t g_period_ns;
static void Sleep_AbsDeadline(int msec)
{
	(void)msec;
	struct timespec ts;
	ts.tv_sec  = (time_t)(g_deadline_ns / 1000000000ull);
	ts.tv_nsec = (long)(g_deadline_ns % 1000000000ull);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/* absolute deadline, waking one guard window early then spinning to the deadline */
static uint64_t g_spin_ns = 50000;   /* 50 us default guard window */
static void Sleep_AbsSpin(int msec)
{
	(void)msec;
	uint64_t t = now_ns();
	if (g_deadline_ns > t + g_spin_ns) {
		uint64_t wake = g_deadline_ns - g_spin_ns;
		struct timespec ts;
		ts.tv_sec  = (time_t)(wake / 1000000000ull);
		ts.tv_nsec = (long)(wake % 1000000000ull);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
	}
	while (now_ns() < g_deadline_ns)
		__asm__ __volatile__("pause" ::: "memory");
}

/* ---- Host_FilterTime, matching host.cpp:663-727 (dedicated-server branch) ----------- */
static int Host_FilterTime(double time)
{
	realtime += sys_timescale * time;

	double fps = sys_ticrate;
	if (fps > 0.0) {
		/* host.cpp:691 — note the fps+1 early-admission bias, and that the original
		   evaluates this in float. Reproduced in float deliberately. */
		if ((float)(1.0f / ((float)fps + 1.0f)) > (float)(realtime - oldrealtime))
			return 0;
	}
	host_frametime = realtime - oldrealtime;
	oldrealtime = realtime;
	return 1;
}

/* ---- simulated per-frame server work ----------------------------------------------- */
static uint64_t g_work_ns = 0;
static void simulate_frame_work(void)
{
	if (!g_work_ns) return;
	uint64_t end = now_ns() + g_work_ns;
	volatile double x = 1.0;
	while (now_ns() < end) { for (int i = 0; i < 64; i++) x = x * 1.0000001 + 1e-9; }
}

/* ---- percentile helpers ------------------------------------------------------------ */
static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}
static double pct(const uint64_t *sorted, size_t n, double p)
{
	if (!n) return 0.0;
	double idx = p * (double)(n - 1);
	size_t lo = (size_t)idx;
	size_t hi = lo + 1 < n ? lo + 1 : lo;
	double f = idx - (double)lo;
	return ((double)sorted[lo] * (1.0 - f) + (double)sorted[hi] * f) / 1000.0; /* -> us */
}

int main(int argc, char **argv)
{
	const char *mode = "old";
	double secs = 10.0;
	int reduce_slack = 0;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--mode=", 7))        mode = argv[i] + 7;
		else if (!strncmp(argv[i], "--ticrate=", 10)) sys_ticrate = atof(argv[i] + 10);
		else if (!strncmp(argv[i], "--secs=", 7))    secs = atof(argv[i] + 7);
		else if (!strncmp(argv[i], "--work-us=", 10)) g_work_ns = (uint64_t)(atof(argv[i] + 10) * 1000.0);
		else if (!strncmp(argv[i], "--spin-us=", 10)) g_spin_ns = (uint64_t)(atof(argv[i] + 10) * 1000.0);
		else if (!strcmp(argv[i], "--reduce-slack")) reduce_slack = 1;
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}

	if (sys_ticrate <= 0.0) { fprintf(stderr, "ticrate must be > 0\n"); return 2; }
	g_period_ns = (uint64_t)(1e9 / sys_ticrate);

	if (reduce_slack && prctl(PR_SET_TIMERSLACK, 1UL) != 0)
		fprintf(stderr, "warning: PR_SET_TIMERSLACK failed: %s\n", strerror(errno));

	/* a real UDP socket, so the Sleep_Net socket-wait path is faithful */
	g_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (g_sock >= 0) {
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = 0;
		bind(g_sock, (struct sockaddr *)&a, sizeof(a));
	}

	void (*sleepfn)(int) = NULL;
	int absolute = 0;
	if      (!strcmp(mode, "old"))     sleepfn = Sleep_Old;        /* -pingboost absent/0 */
	else if (!strcmp(mode, "timer"))   sleepfn = Sleep_Timer;      /* -pingboost 1 */
	else if (!strcmp(mode, "select"))  sleepfn = Sleep_Select;     /* -pingboost 2 */
	else if (!strcmp(mode, "net"))     sleepfn = Sleep_Net;        /* -pingboost 3 */
	else if (!strcmp(mode, "absdeadline")) { sleepfn = Sleep_AbsDeadline; absolute = 1; }
	else if (!strcmp(mode, "absspin"))     { sleepfn = Sleep_AbsSpin;     absolute = 1; }
	else { fprintf(stderr, "unknown mode: %s\n", mode); return 2; }

	if (!strcmp(mode, "timer")) signal(SIGALRM, alarmFunc);

	size_t cap = (size_t)(secs * sys_ticrate * 1.5) + 4096;
	uint64_t *stamps = malloc(cap * sizeof(uint64_t));
	if (!stamps) { fprintf(stderr, "oom\n"); return 1; }

	double m_fOldTime = Sys_FloatTime();
	realtime = oldrealtime = 0.0;

	uint64_t t0 = now_ns();
	uint64_t stop = t0 + (uint64_t)(secs * 1e9);
	g_deadline_ns = t0 + g_period_ns;

	size_t n = 0;
	long iterations = 0, admitted = 0, missed_deadlines = 0;

	struct rusage ru0;
	getrusage(RUSAGE_SELF, &ru0);

	/* sys_ded.cpp:156-177 */
	while (now_ns() < stop) {
		sleepfn(1);                      /* sys_ded.cpp:159 — literal 1 msec */
		iterations++;

		/* sys_engine.cpp:107-113 */
		double m_fCurTime = Sys_FloatTime();
		double m_fFrameTime = m_fCurTime - m_fOldTime;
		m_fOldTime = m_fCurTime;
		if (m_fFrameTime < 0.0) m_fFrameTime = 0.001;

		/* In the absolute modes the deadline IS the admission decision — this is the
		   Phase 2 design, in which the deadline replaces Host_FilterTime's backward-
		   looking gate rather than being bolted on top of it. Keeping both produces a
		   busy-spin whenever the gate rejects a deadline that has already passed, which
		   is itself evidence that the two mechanisms cannot simply coexist. */
		int admit = absolute ? 1 : Host_FilterTime(m_fFrameTime);
		if (!absolute) { /* keep realtime bookkeeping identical in both paths */ }

		if (admit) {
			uint64_t t = now_ns();
			if (n < cap) stamps[n++] = t;
			admitted++;
			simulate_frame_work();
			if (absolute) {
				/* advance the absolute deadline; rebase only if a whole period was lost */
				g_deadline_ns += g_period_ns;
				uint64_t cur = now_ns();
				if (g_deadline_ns < cur) {
					missed_deadlines++;
					g_deadline_ns = cur + g_period_ns;
				}
			}
		}
	}

	if (n < 2) { fprintf(stderr, "too few admitted frames (%zu)\n", n); return 1; }

	size_t ni = n - 1;
	uint64_t *iv = malloc(ni * sizeof(uint64_t));
	for (size_t i = 0; i < ni; i++) iv[i] = stamps[i + 1] - stamps[i];
	qsort(iv, ni, sizeof(uint64_t), cmp_u64);

	double elapsed = (double)(stamps[n - 1] - stamps[0]) / 1e9;
	double achieved = (double)ni / elapsed;
	double target_us = 1e6 / sys_ticrate;

	printf("mode=%-11s ticrate=%-6.0f work_us=%-5.0f achieved_hz=%8.1f target_hz=%7.0f\n",
	       mode, sys_ticrate, g_work_ns / 1000.0, achieved, sys_ticrate);
	printf("  interval_us  p50=%8.1f p95=%8.1f p99=%8.1f p99.9=%9.1f max=%10.1f  (target %.1f)\n",
	       pct(iv, ni, 0.50), pct(iv, ni, 0.95), pct(iv, ni, 0.99),
	       pct(iv, ni, 0.999), (double)iv[ni - 1] / 1000.0, target_us);
	struct rusage ru1;
	getrusage(RUSAGE_SELF, &ru1);
	double cpu = (double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec)
	           + (double)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6
	           + (double)(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec)
	           + (double)(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;
	long vcsw = ru1.ru_nvcsw - ru0.ru_nvcsw;
	long ivcsw = ru1.ru_nivcsw - ru0.ru_nivcsw;

	printf("  cpu=%5.1f%% ctxsw_vol=%-7ld ctxsw_invol=%-5ld\n", 100.0 * cpu / elapsed, vcsw, ivcsw);
	printf("  loop_iterations=%ld admitted=%ld reject_ratio=%.3f missed_deadlines=%ld",
	       iterations, admitted,
	       iterations ? 1.0 - (double)admitted / (double)iterations : 0.0, missed_deadlines);
	if (!strcmp(mode, "net"))
		printf("  net_socketwait=%ld net_blindsleep=%ld blind_pct=%.1f%%",
		       g_net_socketwait, g_net_blindsleep,
		       100.0 * (double)g_net_blindsleep /
		           (double)(g_net_socketwait + g_net_blindsleep ? g_net_socketwait + g_net_blindsleep : 1));
	printf("\n");

	free(iv); free(stamps);
	if (g_sock >= 0) close(g_sock);
	return 0;
}
