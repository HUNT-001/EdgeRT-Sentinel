#!/usr/bin/env bash
# Replay every scenario through riskd over the host transport and check bit-exact results.
# Logic check only: latency from this script says nothing about QNX.
set -euo pipefail
cd "$(dirname "$0")/.."
make -s -C qnx/runtime HOST=1
BIN=qnx/build/host
T=$(mktemp -d); trap 'rm -rf "$T"; kill $RPID 2>/dev/null || true' EXIT
for s in scenarios/*.yaml; do
  n=$(basename "$s" .yaml)
  python3 -m edgert.cli scenario expand "$s" --format csv --out "$T/$n.csv" >/dev/null
  "$BIN/riskd" --name "host_$n" 2>"$T/riskd.log" & RPID=$!
  echo "== $n"
  "$BIN/ipc_test" --csv "$T/$n.csv" --name "host_$n"
  wait $RPID
done
