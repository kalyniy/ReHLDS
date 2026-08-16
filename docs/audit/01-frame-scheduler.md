# Audit 01 — Frame Scheduler and Timing

**Baseline commit:** `0124d56c3d888d922eb045775f71c6682ad1226f` (tag `baseline-x86-32`)
**Date:** 2026-08-15
**Method:** source inspection only. No runtime measurement yet — every quantitative
claim below is marked either **[proven-from-source]** or **[needs-measurement]**.

---

## 1. The actual control flow

The project brief hypothesised that `NET_Sleep_Timeout()` is the scheduler. It is not.
It is one of four interchangeable *sleep primitives*, and it is only reachable via
`-pingboost 3`. The real loop is in the dedicated launcher:

```
rehlds/dedicated/src/sys_ded.cpp:156   while (!bDone)
rehlds/dedicated/src/sys_ded.cpp:159       sys->Sleep(1);            // <-- pacing, ALWAYS msec == 1
rehlds/dedicated/src/sys_ded.cpp:176       bDone = !engineAPI->RunFrame();
                                               |
rehlds/engine/sys_engine.cpp:107               m_fCurTime   = Sys_FloatTime();       // CLOCK_MONOTONIC
rehlds/engine/sys_engine.cpp:108               m_fFrameTime = m_fCurTime - m_fOldTime;
rehlds/engine/sys_engine.cpp:119               Host_Frame(m_fFrameTime, ...)         // double -> float at the boundary
                                                   |
rehlds/engine/host.cpp:872                         if (!Host_FilterTime(time)) return;   // ADMISSION GATE
rehlds/engine/host.cpp:678                             realtime += sys_timescale.value * time;
rehlds/engine/host.cpp:691                             if (1.0f / (fps + 1.0f) > realtime - oldrealtime)
                                                           return FALSE;                 // reject, do nothing
rehlds/engine/host.cpp:726                             host_frametime = realtime - oldrealtime;
rehlds/engine/host.cpp:896                         SV_Frame();                           // the real server frame
```

### Finding 1.1 — There is no deadline anywhere in the system **[proven-from-source]**

The loop is *sleep-then-poll-then-maybe-reject*. Nothing computes a target wake time.
`Host_FilterTime` only asks the backward-looking question "has at least one period
elapsed since the last admitted frame?" A rejected frame does no work and the loop
immediately sleeps again for another fixed quantum.

**Consequence:** achieved frame rate is quantised by the sleep primitive's granularity,
not by `sys_ticrate`. Sleep overshoot is never compensated, because there is no
absolute deadline to catch up to — each admitted frame silently rebases via
`oldrealtime = realtime` (host.cpp:727). Drift is therefore not accumulated, but
neither is it corrected: the server free-runs at whatever rate the sleep primitive
permits, and `sys_ticrate` acts only as an *upper* bound.

This is the single most important structural finding, and it reframes Phase 2: the fix
is not "correct the arithmetic in `NET_Sleep_Timeout`", it is "introduce a deadline
that does not currently exist anywhere."

### Finding 1.2 — `sys->Sleep(1)` hardcodes the quantum **[proven-from-source]**

`rehlds/dedicated/src/sys_ded.cpp:159` passes a literal `1`. `sys_ticrate` is not
consulted by the launcher at all. Three of the four pingboost modes therefore sleep
~1 ms per iteration regardless of the configured tick rate.

---

## 2. The four sleep primitives

Selected once at startup in `Sys_InitPingboost()`, `rehlds/dedicated/src/sys_linux.cpp:149-176`.

| `-pingboost` | Function | Implementation | Line |
|---|---|---|---|
| absent / 0 / other | `Sleep_Old` | `usleep(msec * 1000)` — relative | sys_linux.cpp:78 |
| 1 | `Sleep_Timer` | `setitimer(ITIMER_REAL)` + `pause()`, SIGALRM | sys_linux.cpp:110 |
| 2 | `Sleep_Select` | `select(1, NULL, NULL, NULL, &tv)` — pure timed sleep, no sockets | sys_linux.cpp:85 |
| 3 | `Sleep_Net` | **ignores `msec`**, calls `NET_Sleep_Timeout()` | sys_linux.cpp:96 |

### Finding 2.1 — Modes 0, 1 and 2 make `sys_ticrate > ~1000` unreachable **[proven-from-source, magnitude needs-measurement]**

All three sleep for a *relative* 1 ms (plus kernel timer slack, default 50 µs, plus
scheduler wakeup latency). The next frame can therefore never be admitted sooner than
~1 ms after the loop resumed. At `sys_ticrate 2000` the admission threshold
(500 µs) is *always* already satisfied when the loop wakes, so `Host_FilterTime`
degenerates to "always admit" and the server runs at the sleep rate, not the tick rate.

**Predicted, to be confirmed in Phase 1:** `sys_ticrate 1000` and `sys_ticrate 2000`
produce statistically indistinguishable frame intervals under pingboost 0/1/2, both
landing below 1000 Hz. If measurement contradicts this, this finding is wrong and the
model above must be revised.

### Finding 2.2 — `Sleep_Net` discards its argument **[proven-from-source]**

```c
void Sleep_Net(int msec)
{
    NET_Sleep_Timeout();     // msec unused
}
```
`sys_linux.cpp:96-99`. The timeout is recomputed inside `NET_Sleep_Timeout` from
`sys_ticrate` instead. Mode 3 is thus the only mode whose sleep duration tracks the
configured tick rate — which is why high-tickrate operators use it.

---

## 3. `NET_Sleep_Timeout()` — `rehlds/engine/net_ws.cpp:979-1045`

### Finding 3.1 — Integer truncation, confirmed **[proven-from-source]**

```c
tv.tv_usec = (1000 / fps) * 1000; // TODO: entirely bad code, fix it completely
if (tv.tv_usec <= 0)
    tv.tv_usec = 1;
```
`net_ws.cpp:1006-1008`. The intended value is the period in microseconds,
`1000000 / fps`. The expression written computes that correctly **only when `fps`
divides 1000 exactly**:

| `sys_ticrate` | true period | computed `tv_usec` | error |
|---|---|---|---|
| 100 | 10000 µs | 10000 µs | correct |
| 500 | 2000 µs | 2000 µs | correct |
| 1000 | 1000 µs | 1000 µs | correct |
| 750 | 1333 µs | 1000 µs | 25 % short |
| 1500 | 667 µs | **1 µs** | collapses |
| 2000 | 500 µs | **1 µs** | collapses |

Above 1000, `1000 / fps` truncates to zero *before* the `<= 0` clamp, so the clamp
converts a whole period into a 1 µs timeout. Mode 3 above 1000 Hz is therefore a
**busy-spin through `select()`**, not a sleep. This is consistent with the community
report that pingboost 3 reaches high tick rates at high CPU cost.

Note also that even the "correct" rows are conceptually wrong for a scheduler: the
timeout equals a *whole period*, and it is applied *after* the previous frame's
execution time has already been spent. The server therefore systematically undershoots
its target even when the arithmetic is exact. Fixing the division alone will not
produce a stable 1000 Hz — this reinforces Finding 1.1.

### Finding 3.2 — The stagger logic disables socket waiting most of the time **[proven-from-source]**

```c
static int numFrames;
static int staggerFrames;
...
if (curtime - lasttime > 1) { lasttime = curtime; numFrames = fps; staggerFrames = fps / 100 + 1; }
...
if (numFrames > 0 && numFrames % staggerFrames)
    res = select(number + 1, &fdset, NULL, NULL, &tv);   // waits on sockets, wakes on packet
else
    res = select(0, NULL, NULL, NULL, &tv);              // blind timed sleep, ignores sockets
--numFrames;
```
`net_ws.cpp:981-1044`.

`numFrames` is reset to `fps` roughly once per second and decremented **once per call**.
Under mode 3 at high tick rate the loop iterates far more than `fps` times per second
(1 µs timeouts, per Finding 3.1), so `numFrames` goes negative within a fraction of a
second and the guard `numFrames > 0` fails for the remainder of the interval.

**Consequence:** for most of every second, the server performs a blind sleep that
ignores socket readiness entirely. Incoming packets are not woken on; they are picked
up later by `SV_ReadPackets` during a normally-admitted frame. The "wake early for
network input" property that the brief assumes exists is, in practice, mostly absent.
**[needs-measurement]** — instrument the taken/not-taken ratio of these two branches.

### Finding 3.3 — Division by zero on `sys_ticrate 0` **[proven-from-source]**

`int fps = (int)sys_ticrate.value;` (`net_ws.cpp:985`) is unclamped, and
`sys_ticrate` is registered with no bounds (`host.cpp:59`, default `"100.0"`).
With `-pingboost 3` and `sys_ticrate 0`, `1000 / fps` is an integer division by zero →
SIGFPE. Also `fps / 100 + 1` is safe, but `numFrames % staggerFrames` would divide by
zero if `staggerFrames` were ever 0; it is currently saved only by the short-circuit on
the first call (`numFrames` starts at 0).

Severity: low (requires server-admin misconfiguration, not remote input) but it is a
trivially reachable crash and should be clamped.

---

## 4. `Host_FilterTime()` — `rehlds/engine/host.cpp:663-...`

### Finding 4.1 — The `fps + 1.0f` term **[proven-from-source]**

```c
if (1.0f / (fps + 1.0f) > realtime - oldrealtime)
    return FALSE;
```
`host.cpp:691`. The threshold is the period of `fps + 1`, i.e. deliberately *shorter*
than the nominal period, biasing admission slightly early so the long-run average can
approach `fps` instead of always undershooting. At `fps = 1000` the threshold is
999.0 µs rather than 1000 µs (0.1 % early); at `fps = 100` it is 9901 µs vs 10000 µs
(1 % early). Any replacement scheduler must either reproduce this bias or explicitly
document the behaviour change, since it shifts effective tick rate.

### Finding 4.2 — Timebase precision is adequate **[proven-from-source]**

`realtime`, `oldrealtime`, `host_frametime` are all `double` (`host.cpp:31,42,35`).
`Sys_FloatTime` uses `CLOCK_MONOTONIC` (`sys_dll.cpp:600-613`). There is **no**
float-precision breakdown at 2000 Hz on a long-uptime server. This rules out a
hypothesis worth ruling out before designing the replacement.

One real defect: `Sys_FloatTime` subtracts `start_time.tv_sec` but **not**
`start_time.tv_nsec`:
```c
return (now.tv_sec - start_time.tv_sec) + now.tv_nsec * 0.000000001;
```
`sys_dll.cpp:612`. This leaves a constant offset of 0–1 s. Harmless for deltas (it
cancels), but the value is not what its name implies, and any new code that treats it
as "seconds since start" will be subtly wrong.

### Finding 4.3 — `double` → `float` narrowing at the frame boundary **[proven-from-source]**

`CEngine::Frame` computes `m_fFrameTime` as a `double` (`sys_engine.cpp:108`) and passes
it to `int Host_Frame(float time, ...)` (`host.cpp:951`) and then
`_Host_Frame(float time)` (`host.cpp:869`). The value is a small delta (~5e-4 s), so
`float` holds it with ample relative precision; this is **not** currently a bug, but it
caps the resolution of any future sub-microsecond accounting and should be widened when
the scheduler is reworked.

---

## 5. Implications for Phase 2 design

1. The replacement must introduce an **absolute monotonic deadline**, which does not
   exist today — not merely repair `NET_Sleep_Timeout`'s arithmetic.
2. The launcher's hardcoded `sys->Sleep(1)` must become tick-rate aware, or frame
   pacing must move out of the launcher into the engine entirely. The current split
   (launcher paces, engine admits) is the root cause of the quantisation.
3. Socket wakeup and deadline wakeup must be unified — today they are mutually
   exclusive per iteration (Finding 3.2), and mostly resolve to "no socket wait."
4. `Host_FilterTime`'s `fps + 1` bias is an observable behaviour that must be
   preserved or explicitly changed under a compatibility cvar.
5. Candidate Linux primitives to benchmark: `ppoll`/`epoll_pwait` with a computed
   relative timeout derived from an absolute deadline, `timerfd` armed absolutely
   (`TFD_TIMER_ABSTIME`) added to the same poll set, or `clock_nanosleep` with
   `TIMER_ABSTIME` for the final wait plus a short calibrated spin. Selection must be
   driven by measured p99.9, not by preference.

---

## 6. Open questions requiring measurement (Phase 1)

- Actual frame-interval distribution for pingboost 0/1/2/3 × sys_ticrate 100/500/1000/2000.
- Ratio of socket-waiting vs blind-sleep iterations inside `NET_Sleep_Timeout` (Finding 3.2).
- Wake latency of each primitive on this host (i9-12900K, Ubuntu 24.04, kernel 6.17,
  `PREEMPT_DYNAMIC`), including whether the thread lands on a P-core or an E-core.
- CPU cost of mode 3's spin at 2000 Hz, in cycles and in context switches.
- Whether `Host_FilterTime` rejections are frequent enough to matter as pure overhead.
