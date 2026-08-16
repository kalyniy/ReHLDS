# Competitive 10-Man Server Config — and Why Each Line Is There

Every value below is justified from the engine source, not from forum lore. File and line
references are to this fork.

---

## The one that matters most

**`sv_maxupdaterate` silently overrides your clients' `ex_interp`.**

`SV_SetupMove` derives the interpolation window it rewinds by like this
(`rehlds/engine/sv_user.cpp:1293-1299`):

```c
cl_interptime = _host_client->lastcmd.lerp_msec / 1000.0f;   // what the client asked for
if (cl_interptime > 0.1) cl_interptime = 0.1f;
if (_host_client->next_messageinterval > cl_interptime)      // <-- the trap
    cl_interptime = (float)_host_client->next_messageinterval;
```

and `next_messageinterval = 1.0 / effective_updaterate` (`sv_main.cpp:5373`).

So the interp the server actually rewinds by is:

> **`max(client ex_interp, 1 / effective updaterate)`**

`sv_maxupdaterate` defaults to **30**. A client asking for `cl_updaterate 102` is clamped to
30, `next_messageinterval` becomes 33.3 ms, and the server rewinds by **33.3 ms instead of
the 10 ms the client is actually interpolating**. The rewind lands 23 ms in the past relative
to what the shooter saw. That is a systematic hit-registration error, and no client-side
setting can fix it.

**Rule: `sv_maxupdaterate` must be at least `1 / ex_interp`.** For `ex_interp 0.01` that means
**≥ 100**.

With the client settings you quoted — `rate 100000`, `cl_cmdrate 105`, `cl_updaterate 102`,
`ex_interp 0.01` — and `sv_maxupdaterate 102`: `next_messageinterval` = 9.8 ms, client interp
= 10 ms, 10 > 9.8, so the server uses the client's 10 ms. Aligned.

---

## server.cfg

```
// ---- Rates -------------------------------------------------------------------
// MAX_RATE is 100000 and MIN_RATE 1000 (engine/net.h:108-109); sv_maxrate is clamped
// to MAX_RATE regardless, so 100000 is the true ceiling. 0 means "unlimited", which
// ends up at the same place but says less.
sv_maxrate                100000
sv_minrate                25000

// THE critical pair. See above: these gate the interp the server rewinds by.
// 102 matches cl_updaterate 102 and satisfies ex_interp 0.01.
sv_maxupdaterate          102
sv_minupdaterate          60

// LAN clients take this instead of sv_maxrate. Default 20000 will throttle a LAN
// 10-man badly.
sv_lan_rate               100000

// ---- Tickrate ----------------------------------------------------------------
// Launcher must be:  -pingboost 4 -rtprio 50   (see docs/audit/21)
// Without -pingboost 4, sys_ticrate above 1000 does nothing at all (docs/audit/06).
sys_ticrate               1000

// ---- Lag compensation --------------------------------------------------------
sv_unlag                  1
sv_maxunlag               0.5     // bounds rewind; see note
sv_unlagpush              0.0     // leave at 0 unless you have measured a bias
sv_unlagsamples           1       // most responsive; see note
sv_clienttrace            1       // 1 = studio hitboxes, 0 = bbox only

// ---- ReHLDS command-rate limiter ---------------------------------------------
// Defaults are already far above cl_cmdrate 105; listed so nobody "tunes" them down
// and starts punishing legitimate players (rehlds/rehlds_security.cpp:3-10).
sv_rehlds_movecmdrate_max_avg     1800
sv_rehlds_movecmdrate_max_burst   5500

// ---- Match settings ----------------------------------------------------------
mp_autoteambalance        0
mp_limitteams             0
mp_timelimit              0
mp_freezetime             15
mp_roundtime              1.75
mp_c4timer                35
mp_startmoney             800
mp_buytime                0.25
mp_friendlyfire           1
mp_footsteps              1
mp_flashlight             1
sv_pausable               0
sv_cheats                 0
sv_consistency            1
sv_allowupload            0        // also sidesteps the .hpk custom-content path
sv_allowdownload          0
```

## Launcher

```bash
sudo setcap cap_sys_nice+ep ./hlds_linux          # once
sudo sysctl -w kernel.sched_rt_runtime_us=-1      # once per boot
sudo chmod 666 /dev/cpu_dma_latency               # once per boot
python3 docs/audit/tools/cstate_hold.py 86400 &   # must stay running

taskset -c 2 ./hlds_linux -game cstrike -console -nomaster \
    -pingboost 4 -rtprio 50 +sys_ticrate 1000 +maxplayers 12 +map de_dust2
```

Pin to a P-core and leave its SMT sibling idle. Measured effect of the last three lines:
p99 frame interval 1271 µs → 1005 µs, p99.9 2446 µs → 1009 µs (`docs/audit/21`).

## Client settings these assume

```
rate 100000
cl_updaterate 102
cl_cmdrate 105
ex_interp 0.01
cl_lagcompensation 1
cl_lw 1                 // REQUIRED - see note
```

## Notes on the debatable ones

**`cl_lw` and `cl_lc` are not optional.** `SV_SetupMove` bails out entirely unless both are
set (`sv_user.cpp:1250`): `if (sv_unlag.value == 0.0f || !_host_client->lw || !_host_client->lc) return;`
A client with `cl_lw 0` gets **no lag compensation at all** and will have to lead targets by
their full ping. Worth stating in your match rules.

**`sv_unlagsamples 1`** uses only the newest ping sample (`sv_user.cpp:1115`, capped at 16).
Higher values average more samples: steadier under jittery ping, but slower to track a real
ping change, which means the rewind target is wrong for longer after a change. For a LAN or
low-jitter 10-man, 1 is the responsive choice. Raise to 2 only if players have visibly
unstable ping.

**`sv_maxunlag 0.5`** is the engine default and bounds how far back a shooter can rewind.
Lower values (0.2–0.3) reduce how much a high-ping or fake-lagging player can "shoot into the
past" at everyone else's expense; the cost is that genuinely high-ping players get partial
compensation. For friends on similar connections, 0.5 is fine; for a mixed-ping public game,
0.3 is the more defensible number.

**`sv_unlagpush 0`.** It shifts the rewind target forward in time
(`sv_user.cpp:1303`: `targettime = realtime - latency - interp + sv_unlagpush`). Positive
values rewind *less*. It exists to compensate a measured systematic bias — do not set it by
feel, because you will simply trade one direction of error for the other.

**`sv_clienttrace 1`** selects studio hitboxes; `0` collapses players to bounding boxes. Keep
1 for a real game — but note that `0` is also the cleanest diagnostic if you ever want to test
whether a reg complaint comes from bone reconstruction or from elsewhere.

## Honest limitation of this config

Everything above aligns the *timing* of lag compensation. It cannot fix the defect measured
in `docs/audit/13`: **the rewind restores the victim's `origin` and nothing else** — their
pose (angles, sequence, frame, gait) is read live at trace time. Measured yaw divergence was
19.6° at p90 and 90° at p99.9, on a **LAN client with only a 37 ms rewind window**. Internet
pings make it worse, roughly linearly with the window.

No cvar addresses that. It is an engine fix, and it is still outstanding.
