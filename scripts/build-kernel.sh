#!/usr/bin/env bash
# Build the CMF Phone 1 (Tetris) kernel + MTK vendor modules with plain make,
# mirroring Nothing's Kleaf flow (build.config.mtk_kernel_device_modules).
#
# Usage: build-kernel.sh [config|kernel|modules|ext [resume-from]|all]   (default: all)
set -euo pipefail

P=$(realpath "$(dirname "$0")/..")
SRC=$P/src
OUT=$P/out/kernel
CLANG=$P/toolchain/clang-r487747c/bin
JOBS=${JOBS:-$(nproc)}

export PATH=$CLANG:$PATH
# Paths are relative to the kernel source dir, exactly like build.config.mtk_kernel_device_modules.
DEVICE_MODULES_REL_DIR=../kernel_device_modules-6.1
MAKE_ARGS=(
    O="$OUT" -j"$JOBS"
    ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu-
    KCONFIG_EXT_PREFIX=$DEVICE_MODULES_REL_DIR/
    DEVICE_MODULES_REL_DIR=$DEVICE_MODULES_REL_DIR
    'DEVICE_MODULES_PATH=$(srctree)/$(DEVICE_MODULES_REL_DIR)'
    'DEVCIE_MODULES_INCLUDE=-I$(DEVICE_MODULES_PATH)/include'
    GKI_DEVICE_MODULES_REL_DIR=$DEVICE_MODULES_REL_DIR
    'GKI_DEVICE_MODULES_PATH=$(srctree)/$(DEVICE_MODULES_REL_DIR)'
    'GKI_DEVCIE_MODULES_INCLUDE=-I$(DEVICE_MODULES_PATH)/include'
)
k() { make -C "$SRC/kernel-6.1" "${MAKE_ARGS[@]}" "$@"; }

# Out-of-tree vendor modules, in the order of build.config.mtk_kernel_device_modules
# (dependency order: common -> conninfra -> connfem -> gps/wlan/bt ...).
# *_mt6653 (external combo chip) skipped: not used on Tetris, adaptor_mt6653/Makefile is broken upstream.
# wlan gen4m/6878 + bt/mt66xx come from Kleaf device targets, not the EXT_MODULES list.
# hbt_driver_cus skipped: hbt_driver/ subdir not published by Nothing (perf/telemetry only).
EXT_MODULES="
vendor/mediatek/kernel_modules/connectivity/common
vendor/mediatek/kernel_modules/connectivity/conninfra
vendor/mediatek/kernel_modules/connectivity/connfem
vendor/mediatek/kernel_modules/connectivity/gps/gps_stp
vendor/mediatek/kernel_modules/connectivity/gps/data_link
vendor/mediatek/kernel_modules/connectivity/gps/gps_pwr
vendor/mediatek/kernel_modules/connectivity/gps/gps_scp
vendor/mediatek/kernel_modules/gpu
vendor/mediatek/kernel_modules/connectivity/fmradio
vendor/mediatek/kernel_modules/connectivity/wlan/adaptor
vendor/mediatek/kernel_modules/connectivity/wlan/adaptor/wlan_page_pool
vendor/mediatek/kernel_modules/connectivity/bt/linux_v2
vendor/mediatek/kernel_modules/connectivity/wlan/core/gen4m/build/connac2x/6878
vendor/mediatek/kernel_modules/connectivity/bt/mt66xx
vendor/mediatek/kernel_modules/mtkcam
"

do_config() {
    mkdir -p "$OUT"
    k gki_defconfig
    KCONFIG_CONFIG=$OUT/.config "$SRC/kernel-6.1/scripts/kconfig/merge_config.sh" -m -O "$OUT" \
        "$OUT/.config" \
        "$SRC/kernel_device_modules-6.1/arch/arm64/configs/mgk_64_k61_defconfig" \
        "$P/kernel/linux.config"
    k olddefconfig
    # Report fragment options that didn't survive olddefconfig
    grep -E '^(# )?CONFIG_' "$P/kernel/linux.config" | while read -r l; do
        grep -qxF "$l" "$OUT/.config" || echo "!! not applied: $l"
    done
}

do_kernel() { k Image.gz modules; }

do_modules() {
    k M=$DEVICE_MODULES_REL_DIR modules
}

do_ext() { # do_ext [first-module-to-build]
    export OUT_DIR=$OUT
    local m rel from=${1:-}
    for m in $EXT_MODULES; do
        [[ -n $from && $m != *"$from" ]] && continue; from=
        [[ -d $SRC/$m ]] || { echo "-- skip $m (not in tree)"; continue; }
        rel=$(realpath "$SRC/$m" --relative-to "$SRC/kernel-6.1")
        mkdir -p "$OUT/$rel"
        echo "==== $m"
        # build.config.common sets this; only vendor/ code checks it (Mali version_compat_defs.h),
        # so it's passed here only to avoid rebuilding kernel + device modules.
        # TETRIS_LINUX_HCI=1 (opt-in, crashed boot so far): bt/mt66xx registers an HCI device for BlueZ instead of
        # /dev/stpbt (patches/kernel_modules/0001-bt-mt66xx-optional-linux-hci.patch)
        make -C "$SRC/$m" M="$rel" KERNEL_SRC="$SRC/kernel-6.1" "${MAKE_ARGS[@]}" \
            KCFLAGS=-D__ANDROID_COMMON_KERNEL__ TETRIS_LINUX_HCI="${TETRIS_LINUX_HCI:-0}"
    done
    echo "==== tetris_mdtty"
    make -C "$SRC/kernel-6.1" O="$OUT" -j"$JOBS" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
        CROSS_COMPILE=aarch64-linux-gnu- M="$P/rootfs/src/tetris-mdtty" modules
    echo "==== uleds (torch LED for Phosh)"
    make -C "$SRC/kernel-6.1" O="$OUT" -j"$JOBS" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
        CROSS_COMPILE=aarch64-linux-gnu- M="$P/rootfs/src/tetris-torch" modules
}

case ${1:-all} in
    config)  do_config ;;
    kernel)  do_kernel ;;
    modules) do_modules ;;
    ext)     do_ext "${2:-}" ;;
    all)     do_config; do_kernel; do_modules; do_ext ;;
    *) echo "usage: $0 [config|kernel|modules|ext|all]"; exit 1 ;;
esac
