#!/usr/bin/env bash
# Flash stock Nothing OS (CMF Phone 1 / Tetris, MT6878) via fastboot, non-interactive.
# Partition lists taken from spike0en/nothing_fastboot_flasher (branch galaga-tetris).
#
# Usage: flash-stock.sh <dir-with-extracted-imgs> [--keep-avb]
#   Wipes userdata+metadata, flashes both slots, disables AVB by default (--keep-avb: pristine vbmeta)
#   (needed later for custom kernels), leaves slot A active.
set -euo pipefail

IMG_DIR=${1:?usage: $0 <image-dir> [--keep-avb]}
KEEP_AVB=0
[[ ${2:-} == --keep-avb ]] && KEEP_AVB=1

boot_partitions="boot dtbo init_boot vendor_boot"
firmware_partitions="apusys ccu connsys_bt connsys_gnss connsys_wifi dpm gpueb gz lk logo mcf_ota mcupm modem pi_img scp spmfw sspm tee vcp"
logical_partitions="odm_dlkm odm vendor_dlkm product vendor system_dlkm system_ext system"
vbmeta_partitions="vbmeta_system vbmeta_vendor"

cd "$IMG_DIR"

# fastboot 37's --disable-verity/--disable-verification fails here ("Failed to find
# AVB_MAGIC at offset: 0"), so patch the vbmeta header flags ourselves:
# AvbVBMetaImageHeader.flags (BE u32 @ 0x78) = HASHTREE_DISABLED|VERIFICATION_DISABLED.
VBM_DIR=.
if (( ! KEEP_AVB )); then
    VBM_DIR=$(mktemp -d)
    trap 'rm -rf "$VBM_DIR"' EXIT
    for i in vbmeta $vbmeta_partitions; do
        cp "$i.img" "$VBM_DIR/"
        printf '\x00\x00\x00\x03' | dd of="$VBM_DIR/$i.img" bs=1 seek=120 conv=notrunc status=none
    done
fi
fb() { echo "+ fastboot $*"; fastboot "$@"; }
flash() { # flash <partition> <img> [extra flags]
    local part=$1 img=$2; shift 2
    [[ -f $img ]] || { echo "!! missing $img for $part"; exit 1; }
    fb flash "$@" "$part" "$img"
}

# Sanity: all images present before touching the device
for i in $boot_partitions $firmware_partitions $logical_partitions $vbmeta_partitions vbmeta preloader_raw; do
    [[ -f $i.img ]] || { echo "!! $i.img not found in $IMG_DIR"; exit 1; }
done
[[ $(fastboot getvar product 2>&1 | awk '/product:/{print $2}') == k6878v1_64 ]] \
    || { echo "!! device is not k6878v1_64 (CMF Phone 1)"; exit 1; }

fb --set-active=a
fb erase userdata
fb erase metadata

for s in a b; do
    for i in $boot_partitions $firmware_partitions; do flash "${i}_$s" "$i.img"; done
    flash "preloader_$s" preloader_raw.img
    for i in vbmeta $vbmeta_partitions; do flash "${i}_$s" "$VBM_DIR/$i.img"; done
done

fb reboot fastboot
for _ in $(seq 30); do fastboot devices | grep -q . && break; sleep 2; done
[[ $(fastboot getvar is-userspace 2>&1 | awk '/is-userspace:/{print $2}') == yes ]] \
    || { echo "!! not in fastbootd"; exit 1; }

if [[ -f super_empty.img ]]; then
    fb wipe-super super_empty.img
else
    for i in $logical_partitions; do
        for s in a b; do
            fastboot delete-logical-partition "${i}_$s-cow" 2>/dev/null || true
            fastboot delete-logical-partition "${i}_$s" || true
            fb create-logical-partition "${i}_$s" 1
        done
    done
fi
for i in $logical_partitions; do flash "${i}_a" "$i.img"; done

fb --set-active=a
echo "== Done. Run 'fastboot reboot' to boot Nothing OS."
