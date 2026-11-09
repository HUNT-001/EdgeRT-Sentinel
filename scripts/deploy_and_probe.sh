#!/usr/bin/env bash
# Deploy Gate 1 probes to the running QNX target and collect evidence.
# Run inside the Ubuntu VM after scripts/build_qnx.sh and scripts/run_qemu.sh.
#
#   TARGET_IP=<ip> scripts/deploy_and_probe.sh      (else uses mkqnximage --getip)
#
# Evidence is written to docs/results/gate1/<timestamp>/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/qnx/build/qnx-x86_64"
TARGET_USER="${TARGET_USER:-root}"
SSH_KEY="${EDGERT_SSH_KEY:-$HOME/.ssh/id_ed25519}"
PORT="${LINK_PORT:-5555}"
SSH_OPTS=(-i "$SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)

if [[ -z "${TARGET_IP:-}" ]]; then
  TARGET_IP="$(bash "$ROOT/scripts/run_qemu.sh" --ip | tr -d '[:space:]')"
fi
[[ -n "$TARGET_IP" ]] || { echo "error: no TARGET_IP"; exit 2; }
python3 -c 'import edgert' 2>/dev/null || { echo "error: edgert not importable; activate .venv (pip install -e 'host[dev]')"; exit 2; }
for b in rt_probe link_probe; do [[ -x "$BIN/$b" ]] || { echo "error: $BIN/$b missing; run scripts/build_qnx.sh"; exit 2; }; done

OUT="$ROOT/docs/results/gate1/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
T="$TARGET_USER@$TARGET_IP"
echo "target: $T  evidence: $OUT"

ssh "${SSH_OPTS[@]}" "$T" 'mkdir -p /tmp/edgert'
scp "${SSH_OPTS[@]}" -q "$BIN/rt_probe" "$BIN/link_probe" "$T:/tmp/edgert/"
ssh "${SSH_OPTS[@]}" "$T" 'uname -a; pidin info 2>/dev/null | head -5' | tee "$OUT/target_info.txt"

echo "== rt_probe (50 Hz, 3000 cycles = 60 s) =="
set +e
ssh "${SSH_OPTS[@]}" "$T" '/tmp/edgert/rt_probe --hz 50 --cycles 3000 --prio 40' | tee "$OUT/rt_probe.txt"
RT_RC=${PIPESTATUS[0]}

echo "== link_probe over TCP :$PORT =="
ssh "${SSH_OPTS[@]}" "$T" "/tmp/edgert/link_probe --port $PORT --hz 20 --count 200" 2>"$OUT/link_probe_target.log" &
sleep 1
python3 -m edgert.cli probe --host "$TARGET_IP" --port "$PORT" --expect 200 --hz 20 | tee "$OUT/link_probe.txt"
LINK_RC=${PIPESTATUS[0]}
wait
set -e

{ echo "rt_probe_rc=$RT_RC"; echo "link_probe_rc=$LINK_RC"; } > "$OUT/summary.txt"
if [[ $RT_RC -eq 0 && $LINK_RC -eq 0 ]]; then
  echo "GATE 1 (target side): PASS" | tee -a "$OUT/summary.txt"
else
  echo "GATE 1 (target side): FAIL — see $OUT" | tee -a "$OUT/summary.txt"
  echo "If TCP failed, use the serial fallback: run '/tmp/edgert/link_probe --stdout' on the target console."
  exit 1
fi
