#!/usr/bin/env bash
# Extract the stock vendor partition (erofs) to out/vendor-extract/vendor.
# Needed for /vendor/firmware (conninfra.cfg, connfem.cfg, wifi.cfg, mali_csffw.bin, SCP, ...).
set -euo pipefail
P=$(realpath "$(dirname "$0")/..")
IMG=${1:-$P/firmware/Tetris_B4.1-260928-1140/images/vendor.img}
O=$P/out/vendor-extract
rm -rf "$O/vendor"; mkdir -p "$O"
fsck.erofs --extract="$O/vendor" --no-preserve "$IMG" >/dev/null
echo "vendor firmware: $(ls "$O/vendor/firmware" | wc -l) files"
