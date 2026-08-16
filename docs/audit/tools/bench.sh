#!/bin/bash
# Reproducible competitive-load benchmark for ReHLDS scheduler work.
#
# Runs a pinned dedicated server with N zBots on a fixed map, lets it reach steady state,
# then samples the frame instrumentation (sv_rehlds_perf_frame) and the process's CPU and
# context-switch counters over a fixed window.
#
# Usage:  ./bench.sh <label> <sys_ticrate> [extra hlds args...]
#   e.g.  ./bench.sh 2000_pb4 2000 -pingboost 4
#
# Prerequisites (see docs/audit/06-real-server-validation.md and 08-load-benchmark.md):
#   - HLDS + cstrike installed at $HLDS_DIR
#   - ReGameDLL cs.so deployed to cstrike/dlls/
#   - zBot profiles extracted and bot_enable 1 in cstrike/game_init.cfg
#   - maps/<map>.nav generated (the first bot spawn on a navless map generates it)

set -u

HLDS_DIR="${HLDS_DIR:-/home/dan/projects/hlds}"
STEAM_LIB="${STEAM_LIB:-/home/dan/projects/steamcmd/linux32}"
MAP="${MAP:-de_dust2}"
BOTS="${BOTS:-10}"
CORE="${CORE:-2}"
PORT="${PORT:-27500}"
WARMUP="${WARMUP:-40}"   # seconds for bots to join, buy and engage
SAMPLE="${SAMPLE:-25}"   # measured window

LABEL="${1:?usage: bench.sh <label> <ticrate> [extra args...]}"
TICRATE="${2:?usage: bench.sh <label> <ticrate> [extra args...]}"
shift 2

OUT="${OUT_DIR:-/tmp/rehlds-bench}"
mkdir -p "$OUT"
LOG="$OUT/$LABEL.log"

cd "$HLDS_DIR" || exit 1

# Bot and match settings are applied from server.cfg so every run starts identically.
cat > cstrike/server.cfg <<EOF
hostname "ReHLDS bench $LABEL"
sv_lan 1
mp_autoteambalance 1
mp_limitteams 0
mp_timelimit 0
mp_freezetime 0
mp_roundtime 9
bot_join_after_player 0
bot_quota_mode normal
bot_quota $BOTS
bot_difficulty 2
${EXTRA_CVARS:-}
EOF

{
	sleep "$WARMUP"
	echo "sv_rehlds_perf_frame 1"
	echo "rehlds_perf_frame_reset"
	sleep "$SAMPLE"
	echo "status"
	echo "rehlds_perf_frame_dump"
	sleep 3
	echo "quit"
	sleep 3
} | LD_LIBRARY_PATH="$STEAM_LIB:${LD_LIBRARY_PATH:-}" \
	taskset -c "$CORE" ./hlds_linux -game cstrike -console -nomaster -insecure \
	+sv_lan 1 +map "$MAP" +maxplayers $((BOTS + 2)) +sys_ticrate "$TICRATE" \
	-port "$PORT" "$@" > "$LOG" 2>&1 &

SRV=$!

# Sample CPU and context switches across the same window the instrument covers.
sleep $((WARMUP + 1))
HZ=$(getconf CLK_TCK)
read -r cpu_a < <(awk '{print $14+$15}' "/proc/$SRV/stat" 2>/dev/null || echo 0)
csw_a=$(awk '/^voluntary_ctxt|^nonvoluntary_ctxt/{s+=$2} END{print s+0}' "/proc/$SRV/status" 2>/dev/null)
sleep $((SAMPLE - 2))
read -r cpu_b < <(awk '{print $14+$15}' "/proc/$SRV/stat" 2>/dev/null || echo 0)
csw_b=$(awk '/^voluntary_ctxt|^nonvoluntary_ctxt/{s+=$2} END{print s+0}' "/proc/$SRV/status" 2>/dev/null)

wait $SRV 2>/dev/null

WIN=$((SAMPLE - 2))
echo "===== $LABEL (ticrate=$TICRATE $*) ====="
grep -E "^players|^#.*[0-9]+ +[0-9]+" "$LOG" | head -1
sed -n '/frameperf:/,/^  deadline source\|^        target one/p' "$LOG"
if [ -n "${cpu_a:-}" ] && [ -n "${cpu_b:-}" ]; then
	echo "  cpu=$(echo "scale=1; ($cpu_b-$cpu_a)*100/$HZ/$WIN" | bc)%  ctxsw_per_sec=$(echo "scale=0; ($csw_b-$csw_a)/$WIN" | bc)"
fi
echo
