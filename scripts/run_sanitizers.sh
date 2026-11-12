#!/usr/bin/env bash
# ASan+UBSan over the C++ tests, plus 60k full-range golden-vs-C++ vectors. Needs g++, python3.
set -euo pipefail
cd "$(dirname "$0")/.."
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
S="-std=gnu++17 -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1"
I="-Iqnx/common/include -Iaccelerator/include"
python3 accelerator/testbench/generate_vectors.py >/dev/null
python3 tests/fuzz/gen_full_range.py "$B/full.csv"
g++ $S -Wno-unknown-pragmas -Wno-unused-label $I -Iaccelerator/vectors accelerator/testbench/tb_risk.cpp \
    accelerator/src/risk_core.cpp accelerator/src/risk_kernel.cpp -o "$B/tb"
"$B/tb" accelerator/vectors/directed.csv accelerator/vectors/random_seeded.csv "$B/full.csv"
g++ $S $I tests/unit/test_protocol.cpp accelerator/src/risk_core.cpp -o "$B/tp" && "$B/tp"
g++ $S $I tests/unit/test_control.cpp -o "$B/tc"
mkdir -p "$B/csv"
for s in scenarios/*.yaml; do
  python3 -m edgert.cli scenario expand "$s" --format csv --out "$B/csv/$(basename "$s" .yaml).csv" >/dev/null
done
"$B/tc" "$B/csv"
echo "SANITIZERS PASS"
