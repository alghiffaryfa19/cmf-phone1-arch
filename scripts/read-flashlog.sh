#!/usr/bin/env bash
# Read the debug initramfs' persistent log from nt_reserve2 (needs Nothing OS + Magisk root via adb).
# Usage: read-flashlog.sh <outdir>
set -euo pipefail
O=${1:?outdir}; mkdir -p "$O"
adb shell 'su -c "dd if=/dev/block/by-name/nt_reserve2 bs=1M count=16 2>/dev/null"' | tr -d '\000' > "$O/init.log"
adb shell 'su -c "dd if=/dev/block/by-name/nt_reserve2 bs=1M skip=16 count=2 2>/dev/null"' | tr -d '\000' > "$O/dmesg.txt"
wc -l "$O/init.log" "$O/dmesg.txt"
