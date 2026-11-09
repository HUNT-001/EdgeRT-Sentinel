#!/usr/bin/env bash
# Create (first run) and boot the QNX 8.0 x86_64 QEMU target with mkqnximage.
# Run inside the Ubuntu VM. Cross-check flags against the official QNX QEMU
# codelab if your SDP build differs:  mkqnximage --help
#
#   scripts/run_qemu.sh          # build image if missing, then boot
#   scripts/run_qemu.sh --rebuild
#   scripts/run_qemu.sh --ip     # print target IP of a running VM
set -euo pipefail

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
VM_DIR="${EDGERT_VM_DIR:-$HOME/edgert-qnxvm}"
SSH_KEY="${EDGERT_SSH_KEY:-$HOME/.ssh/id_ed25519}"

if [[ -z "${QNX_TARGET:-}" ]]; then
  # shellcheck disable=SC1091
  source "$QNX_SDP/qnxsdp-env.sh" >/dev/null
fi
command -v mkqnximage >/dev/null || { echo "error: mkqnximage not on PATH (is the SDP env sourced?)"; exit 2; }

mkdir -p "$VM_DIR"
cd "$VM_DIR"

if [[ "${1:-}" == "--ip" ]]; then
  mkqnximage --getip
  exit 0
fi

[[ -f "$SSH_KEY.pub" ]] || ssh-keygen -t ed25519 -N "" -f "$SSH_KEY"

if [[ "${1:-}" == "--rebuild" ]] || [[ ! -d output ]]; then
  mkqnximage --clean >/dev/null 2>&1 || true
  mkqnximage --type=qemu --arch=x86_64 --ssh-ident="$SSH_KEY.pub"
fi

echo "Booting QNX target. Get its IP from another shell: scripts/run_qemu.sh --ip"
mkqnximage --run
