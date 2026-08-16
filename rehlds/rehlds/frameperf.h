/*
* Frame scheduler instrumentation for ReHLDS.
*
* Records, per iteration of the dedicated server's host loop, when the loop woke, whether
* Host_FilterTime admitted a frame, and how long an admitted frame took to execute. The
* samples land in a fixed-size ring buffer with no formatting and no I/O on the hot path;
* percentiles are computed only when the operator asks for them.
*
* Motivation and measured baseline: docs/audit/01-frame-scheduler.md and
* docs/audit/05-scheduler-measurements.md. In short, the engine has no frame deadline of
* any kind, so "are we hitting sys_ticrate" cannot be answered from existing counters. The
* legacy FPS display reports an average and hides exactly the tail behaviour that matters
* for competitive play.
*
* The intended deadline this file reports is synthetic: it is derived from sys_ticrate at
* the moment sampling was enabled. Nothing in the engine schedules against it. It exists so
* that lateness can be quantified against a target the engine does not currently pursue.
*
* Disabled by default and costs nothing when off (one predictable branch per call site).
*/

#pragma once

#include "maintypes.h"

void FramePerf_Init();

// Top of the host loop iteration, immediately after the pingboost sleep returns.
void FramePerf_OnWake();

// Result of the Host_FilterTime admission gate for this iteration.
void FramePerf_OnAdmission(qboolean admitted);

// Bracket the admitted frame's work.
void FramePerf_OnFrameBegin();
void FramePerf_OnFrameEnd();

// Per-subsystem timing inside SV_Frame. These are the stages the brief ranks highest
// (sections 5 and 11): the command path, per-frame physics, and snapshot/delta encoding.
// perf(1) would be the usual tool, but it needs perf_event_paranoid lowered, which needs
// root; these timers need no privileges and attribute directly to engine stages rather
// than to symbols.
enum FramePerfSubsystem
{
	FP_SUB_READPACKETS = 0,	// SV_ReadPackets: parse move, SV_RunCmd, lag comp, weapons, traces
	FP_SUB_PHYSICS,		// SV_Physics + pfnStartFrame
	FP_SUB_SNAPSHOT,	// SV_SendClientMessages: fullpack, delta encode, netchan
	FP_SUB_STARTFRAME,	// gEntityInterface.pfnStartFrame, called from inside SV_Physics
	FP_SUB_COUNT
};

void FramePerf_SubBegin(int subsystem);
void FramePerf_SubEnd(int subsystem);

// Per-frame edict accounting for SV_Physics. Separates the fixed cost of walking every
// edict from the work actually done, which is what decides whether raising sys_ticrate is
// affordable: measured, per-frame physics cost fell only 13% when the frame rate doubled.
void FramePerf_PhysicsCounts(uint32 visited, uint32 simulated);
