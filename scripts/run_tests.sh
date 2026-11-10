#!/usr/bin/env bash
# Run every off-target test. Needs: g++, python3 with pytest, typer, pyyaml.
# On the QNX target itself the probes are run by deploy_and_probe.sh instead.
set -euo pipefail
cd "$(dirname "$0")/.."
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT

echo "== protocol contract (compile-time layout + round trip)"
g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -Iqnx/common/include -Iaccelerator/include \
    tests/unit/test_protocol.cpp accelerator/src/risk_core.cpp -o "$B/test_protocol"
"$B/test_protocol"

echo "== RACE C++ vs golden model (directed + 10000 random)"
python3 accelerator/testbench/generate_vectors.py >/dev/null
g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-unknown-pragmas -Wno-unused-label -Iaccelerator/include -Iaccelerator/vectors \
    accelerator/testbench/tb_risk.cpp accelerator/src/risk_core.cpp accelerator/src/risk_kernel.cpp -o "$B/tb_risk"
"$B/tb_risk" accelerator/vectors/directed.csv accelerator/vectors/random_seeded.csv
"$B/tb_risk"

echo "== python: golden model properties + host CLI/scenarios"
python3 -m pytest -q tests/unit
(cd host && PYTHONPATH=. python3 -m pytest -q)

echo "== control policy + safety monitor (unit + scenario invariants)"
g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -Iqnx/common/include -Iaccelerator/include \
    tests/unit/test_control.cpp -o "$B/test_control"
mkdir -p "$B/csv"
for s in scenarios/*.yaml; do
  python3 -m edgert.cli scenario expand "$s" --format csv --out "$B/csv/$(basename "$s" .yaml).csv" >/dev/null
done
"$B/test_control" "$B/csv"

echo "== riskd IPC replay of all scenarios (host transport)"
bash scripts/ipc_test_host.sh | grep -c "IPC TEST: PASS" | grep -qx "$(ls scenarios/*.yaml | wc -l)"

echo "ALL TESTS PASS"
