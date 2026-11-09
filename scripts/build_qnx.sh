#!/usr/bin/env bash
# Build Gate 1 probes for QNX 8.0 x86_64. Run inside the Ubuntu VM.
set -euo pipefail

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -z "${QNX_TARGET:-}" ]]; then
  [[ -f "$QNX_SDP/qnxsdp-env.sh" ]] || { echo "error: $QNX_SDP/qnxsdp-env.sh not found (set QNX_SDP)"; exit 2; }
  # shellcheck disable=SC1091
  source "$QNX_SDP/qnxsdp-env.sh" >/dev/null
fi
command -v q++ >/dev/null || { echo "error: q++ not on PATH after sourcing SDP env"; exit 2; }

make -C "$ROOT/qnx/gate1"
file "$ROOT"/qnx/build/qnx-x86_64/* 2>/dev/null || ls -l "$ROOT"/qnx/build/qnx-x86_64/
