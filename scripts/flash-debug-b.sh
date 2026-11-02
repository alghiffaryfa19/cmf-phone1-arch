#!/usr/bin/env bash
# Flash the debug kernel + initramfs to slot b and boot it. Slot a (Nothing OS) is untouched;
# go back with: fastboot --set-active=a && fastboot reboot
# --shell: flash the tetris.debug=1 vendor_boot (always stays in the telnet initramfs)
set -euo pipefail
OUT=$(realpath "$(dirname "$0")/../out")
if adb get-state >/dev/null 2>&1; then adb reboot bootloader; fi
for _ in $(seq 30); do fastboot devices | grep -q . && break; sleep 2; done
fastboot flash boot_b "$OUT/debug-boot.img"
fastboot flash init_boot_b "$OUT/debug-init_boot.img"
VB=debug-vendor_boot.img; [[ ${1:-} == --shell ]] && VB=debug-vendor_boot-shell.img
fastboot flash vendor_boot_b "$OUT/$VB"
fastboot --set-active=b
fastboot reboot
