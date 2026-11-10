#!/usr/bin/env bash
# RACE kernel HLS flow: csim -> synth -> cosim. Load Vitis first:
#   source <install>/2026.1/Vitis/settings64.sh
# Reports are copied to accelerator/reports/.
set -euo pipefail
cd "$(dirname "$0")"
WORK=build_risk
command -v vitis-run >/dev/null || { echo "error: vitis-run not on PATH - source Vitis settings64.sh"; exit 2; }
echo "== C simulation ==";        vitis-run --mode hls --csim  --config risk_hls.cfg --work_dir "$WORK"
echo "== C synthesis ==";         v++ -c --mode hls             --config risk_hls.cfg --work_dir "$WORK"
echo "== C/RTL co-simulation =="; vitis-run --mode hls --cosim --config risk_hls.cfg --work_dir "$WORK"
mkdir -p ../reports
find "$WORK" \( -name '*_csynth.rpt' -o -name '*_cosim.rpt' \) -exec cp {} ../reports/ \;
echo "RISK HLS FLOW: PASS (reports in accelerator/reports)"
