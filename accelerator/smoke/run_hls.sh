#!/usr/bin/env bash
# Gate 1 HLS smoke flow on Linux: csim -> synth -> cosim.
# Load Vitis first:  source <install>/2026.1/Vitis/settings64.sh
#   ./run_hls.sh          full flow
#   ./run_hls.sh --gcc    g++-only testbench (no Vitis needed)
set -euo pipefail
cd "$(dirname "$0")"
WORK=build_smoke

if [[ "${1:-}" == "--gcc" ]]; then
  mkdir -p "$WORK"
  g++ -std=c++17 -O2 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-label -o "$WORK/tb_gcc" tb_smoke.cpp smoke_kernel.cpp
  exec "$WORK/tb_gcc"
fi

command -v vitis-run >/dev/null || { echo "error: vitis-run not on PATH - source Vitis settings64.sh"; exit 2; }
echo "== C simulation ==";        vitis-run --mode hls --csim  --config hls_config.cfg --work_dir "$WORK"
echo "== C synthesis ==";         v++ -c --mode hls             --config hls_config.cfg --work_dir "$WORK"
echo "== C/RTL co-simulation =="; vitis-run --mode hls --cosim --config hls_config.cfg --work_dir "$WORK"

mkdir -p reports
find "$WORK" \( -name '*_csynth.rpt' -o -name '*_cosim.rpt' \) -exec cp {} reports/ \;
echo "GATE1 HLS FLOW: PASS  (reports in accelerator/smoke/reports)"
