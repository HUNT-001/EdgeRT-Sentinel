#!/usr/bin/env bash
# Put the runtime binaries and scenario CSVs into the QEMU image (sshd is not usable, so we bake).
# Run in the Ubuntu VM after scripts/build_qnx.sh. Rebuilds the image; then boot with run_qemu.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${IMG:-$HOME/edgert-qnxvm}"
BIN="$ROOT/qnx/build/qnx-x86_64"
QNX_SDP="${QNX_SDP:-$HOME/qnx800}"

for b in rt_probe link_probe riskd ipc_test; do [[ -f "$BIN/$b" ]] || { echo "error: $BIN/$b missing; run scripts/build_qnx.sh"; exit 2; }; done
[[ -d "$IMG" ]] || { echo "error: image dir $IMG not found (set IMG)"; exit 2; }
[[ -n "${QNX_TARGET:-}" ]] || source "$QNX_SDP/qnxsdp-env.sh" >/dev/null

STAGE="$IMG/edgert_stage"; rm -rf "$STAGE"; mkdir -p "$STAGE"
for s in "$ROOT"/scenarios/*.yaml; do
  python3 -m edgert.cli scenario expand "$s" --format csv --out "$STAGE/$(basename "$s" .yaml).csv" >/dev/null
done

SNIP="$IMG/local/snippets/data_files.custom"; mkdir -p "$(dirname "$SNIP")"
{
  for b in rt_probe link_probe riskd ipc_test; do echo "[uid=0 gid=0 perms=755] home/root/$b=$BIN/$b"; done
  for c in "$STAGE"/*.csv; do echo "[uid=0 gid=0 perms=644] home/root/$(basename "$c")=$c"; done
} > "$SNIP"
echo "snippet:"; cat "$SNIP"

cd "$IMG"
mkqnximage --type=qemu --arch=x86_64 --build
echo
echo "Image rebuilt. Boot: scripts/run_qemu.sh  then on the QNX console:  cd /data/home/root  (or ~)"
