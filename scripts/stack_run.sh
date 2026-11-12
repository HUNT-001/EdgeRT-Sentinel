#!/bin/sh
# Launch the whole pipeline on one machine (QNX target shell or Linux host) and replay a scenario.
#   stack_run.sh BIN_DIR SCENARIO.csv [motiond args, e.g. --crash-after 50]
# Env: LOGDIR (default /tmp/edgert_stack)   RT=1 -> SCHED_FIFO priorities (QNX)   TAIL_MS (default 1500)
# POSIX sh only: runs under QNX ksh and bash. Prints all logs at the end.
BIN=$1; CSV=$2
[ -x "$BIN/motiond" ] && [ -f "$CSV" ] || { echo "usage: stack_run.sh BIN_DIR SCENARIO.csv [motiond args]"; exit 2; }
shift 2
LOGDIR=${LOGDIR:-/tmp/edgert_stack}; TAIL_MS=${TAIL_MS:-1500}
mkdir -p "$LOGDIR" && rm -f "$LOGDIR"/*.log "$LOGDIR"/*.jsonl

if [ -n "$RT" ]; then
  P_TELE="--prio 20"; P_SAFE="--prio 60"; P_ACT="--prio 55"; P_RISK="--prio 50"; P_MOT="--prio 50"; P_SENS="--prio 30"
fi

PIDS=""
cleanup() { for p in $PIDS; do kill $p 2>/dev/null; done; }
trap cleanup EXIT INT TERM

"$BIN/telemetryd" $P_TELE > "$LOGDIR/telemetry.jsonl" 2> "$LOGDIR/telemetryd.log" & PIDS="$PIDS $!"
sleep 0.3 2>/dev/null || sleep 1
"$BIN/actuatord" $P_ACT > "$LOGDIR/actuatord.log" 2>&1 & PIDS="$PIDS $!"
"$BIN/safetyd" $P_SAFE --grace-ms 4000 > "$LOGDIR/safetyd.log" 2>&1 & SAFE=$!; PIDS="$PIDS $SAFE"
"$BIN/riskd" $P_RISK --persist > "$LOGDIR/riskd.log" 2>&1 & PIDS="$PIDS $!"
sleep 1
"$BIN/motiond" $P_MOT "$@" > "$LOGDIR/motiond.log" 2>&1 & MOT=$!; PIDS="$PIDS $MOT"
sleep 1
"$BIN/sensor_replayd" $P_SENS --csv "$CSV" --tail-ms "$TAIL_MS" > "$LOGDIR/sensor.log" 2>&1
RC=$?

# Stop order: safetyd first (so a normal shutdown is not reported as a heartbeat loss), then motiond so
# its summary prints, then the rest.
kill $SAFE 2>/dev/null; sleep 1
kill $MOT 2>/dev/null; sleep 1
cleanup; sleep 1

for f in actuatord safetyd riskd motiond sensor; do echo "--- $f"; cat "$LOGDIR/$f.log"; done
echo "--- telemetry ($(wc -l < "$LOGDIR/telemetry.jsonl") events, full stream in $LOGDIR/telemetry.jsonl)"; cat "$LOGDIR/telemetryd.log"
exit $RC
