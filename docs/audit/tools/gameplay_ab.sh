#!/bin/bash
# Statistical gameplay A/B for scheduler changes.
#
# Bot behaviour is not deterministic, so a single run of A against a single run of B proves
# nothing. This runs each condition twice so that within-condition variance can be compared
# against the between-condition difference. If the A-vs-A spread is as large as the A-vs-B
# gap, the result is noise -- which is the answer this script exists to make visible.
#
# Detects gross gameplay regressions only. It cannot exercise lag compensation, because
# SV_SetupMove is gated on !host_client->fakeclient (engine/sv_user.cpp:808) and every bot
# is a fake client. Real-client testing is required for that; see docs/audit/09.
#
# Usage: ./gameplay_ab.sh

set -u
HLDS_DIR="${HLDS_DIR:-/home/dan/projects/hlds}"
STEAM_LIB="${STEAM_LIB:-/home/dan/projects/steamcmd/linux32}"
SAMPLE="${SAMPLE:-120}"
WARMUP="${WARMUP:-45}"
OUT="${OUT:-/tmp/rehlds-ab}"
mkdir -p "$OUT"
cd "$HLDS_DIR" || exit 1

cat > cstrike/server.cfg <<'EOF'
hostname "ReHLDS A/B"
sv_lan 1
mp_timelimit 0
mp_freezetime 0
mp_roundtime 9
bot_join_after_player 0
bot_quota 10
bot_difficulty 2
sv_unlag 1
EOF

run() {
	local tag="$1"; shift
	{
		sleep "$WARMUP"
		echo "sv_rehlds_perf_hitreg 1"
		echo "rehlds_perf_hitreg_reset"
		echo "sv_rehlds_perf_frame 1"
		echo "rehlds_perf_frame_reset"
		sleep "$SAMPLE"
		echo "rehlds_perf_hitreg_dump"
		echo "rehlds_perf_frame_dump"
		sleep 3
		echo "quit"
		sleep 3
	} | LD_LIBRARY_PATH="$STEAM_LIB:${LD_LIBRARY_PATH:-}" \
		taskset -c 2 ./hlds_linux -game cstrike -console -nomaster -insecure \
		+sv_lan 1 +map de_dust2 +maxplayers 12 +sys_ticrate 1000 \
		-port 27500 "$@" > "$OUT/$tag.log" 2>&1

	local hulls hits ratio hitsum ach
	hulls=$(grep -oP 'cache hit=\K[0-9]+' "$OUT/$tag.log" | tail -1)
	local miss; miss=$(grep -oP 'miss=\K[0-9]+' "$OUT/$tag.log" | tail -1)
	ratio=$(grep -oP 'hit_ratio=\K[0-9.]+' "$OUT/$tag.log" | tail -1)
	hitsum=$(grep -oP 'player hits by hitgroup \(\K[0-9]+' "$OUT/$tag.log" | tail -1)
	ach=$(grep -oP 'achieved=\K[0-9.]+' "$OUT/$tag.log" | tail -1)
	printf '%-14s achieved=%-8s player_hits=%-6s studio_hulls=%-6s cache_hit_ratio=%s%%\n' \
		"$tag" "${ach:-?}" "${hitsum:-?}" "$(( ${hulls:-0} + ${miss:-0} ))" "${ratio:-?}"
	printf '   hitgroups: '
	sed -n '/player hits by hitgroup/,/^---\|^$/p' "$OUT/$tag.log" | \
		grep -oP '^\s+\K[a-z ]+\s+[0-9]+\s+[0-9.]+' | \
		awk '{printf "%s=%s ", $1$2, $(NF-1)}'
	echo
}

echo "=== within-condition variance (same build, same settings, two runs) ==="
run pb0_a
run pb0_b
echo "=== candidate ==="
run pb4_a -pingboost 4
run pb4_b -pingboost 4
