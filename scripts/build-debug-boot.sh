#!/usr/bin/env bash
# Build the debug boot set for slot b:
#   out/debug-boot.img         kernel only                         -> boot_b
#   out/debug-vendor_boot.img  stock DTB/cmdline + debug initramfs -> vendor_boot_b
#   out/debug-init_boot.img    empty ramdisk                       -> init_boot_b
# The bootloader ignores boot.img's ramdisk on this device and concatenates
# vendor_boot ramdisk + init_boot ramdisk. init_boot is only 8M, so our initramfs
# (busybox + both module stages) lives in vendor_boot (64M) and init_boot is
# emptied so Android's /init can't override ours.
# LK has no `fastboot boot`; use scripts/flash-debug-b.sh.
set -euo pipefail

P=$(realpath "$(dirname "$0")/..")
OUT=$P/out
KOUT=$OUT/kernel
KVER=$(cat "$KOUT/include/config/kernel.release")
STAGE1=$P/stock-info/unpacked/vendor_boot_vendor_ramdisk00.d/lib/modules/modules.load
STAGE2=$P/stock-info/vendor_dlkm/modules.load
STOCK_VB=$P/firmware/Tetris_B4.1-260928-1140/images/vendor_boot.img
BUSYBOX=$P/downloads/alarm/usr/bin/busybox
STRIP=$P/toolchain/clang-r487747c/bin/llvm-strip
R=$OUT/debug-initramfs
MD=$R/lib/modules/$KVER

rm -rf "$R"
mkdir -p "$R"/{bin,dev,proc,sys,tmp,run} "$MD"
install -m755 "$BUSYBOX" "$R/bin/busybox"
install -m755 "$P/initramfs/init" "$R/init"
# Stock /vendor/firmware (firmware_class.path=/vendor/firmware): conninfra.cfg etc. are read
# at module load, before the rootfs exists. From scripts/extract-vendor.sh.
[[ -d $OUT/vendor-extract/vendor/firmware ]] || "$P/scripts/extract-vendor.sh"
mkdir -p "$R/vendor"; cp -a "$OUT/vendor-extract/vendor/firmware" "$R/vendor/"
# reboot-mode bootloader|recovery|...: busybox reboot can't pass a reboot argument
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu -O2 -static -nostdlib \
    -ffreestanding -fno-stack-protector -fuse-ld=lld -o "$R/bin/reboot-mode" "$P/initramfs/tools/reboot-mode.c"

# Index every built module by basename
declare -A KO
while IFS= read -r f; do KO[$(basename "$f")]=$f; done < <(find "$OUT" -path "$R" -prune -o -name '*.ko' -print)

# Stage lists in stock order (vendor_boot first stage, then vendor_dlkm second stage)
BLOCK=$P/initramfs/modules.blocklist
add_stage() { # add_stage <stock modules.load> <our list name>
    local m missing=0
    : > "$MD/$2"
    while IFS= read -r m; do
        m=$(basename "$m"); [[ -z $m ]] && continue
        grep -qxF "$m" "$BLOCK" && { echo "-- $2: blocked: $m"; continue; }
        if [[ -n ${KO[$m]:-} ]]; then
            [[ -f $MD/$m ]] || "$STRIP" --strip-debug -o "$MD/$m" "${KO[$m]}"
            echo "$m" >> "$MD/$2"
        else
            echo "-- $2: not built: $m"; missing=$((missing+1))
        fi
    done < "$1"
    echo "$2: $(wc -l < "$MD/$2") modules (missing $missing)"
}
add_stage "$STAGE1" modules.load
add_stage "$STAGE2" modules.load.2
add_stage <(grep -vE '^#|^$' "$P/initramfs/modules.load.3") modules.load.3

# GKI modules (stock: system_dlkm, e.g. rfkill, libarc4) so modprobe can pull them as deps
while IFS= read -r f; do
    m=$(basename "$f"); [[ -f $MD/$m ]] || "$STRIP" --strip-debug -o "$MD/$m" "$f"
done < <(find "$KOUT" -name '*.ko')

# Stock order isn't dependency order (Android's init modprobes via modules.dep),
# so generate modules.dep for our set and let busybox modprobe resolve deps.
depmod -b "$R" -F "$KOUT/System.map" -e "$KVER" 2>&1 | grep -v 'needs unknown symbol' | head -20 || true
echo "unresolved symbols: $(depmod -n -b "$R" -F "$KOUT/System.map" -e "$KVER" 2>&1 >/dev/null | grep -c 'needs unknown symbol')"

(cd "$R" && find . | sort | cpio -o -H newc -R 0:0 --quiet) | lz4 -l -12 --favor-decSpeed -q > "$OUT/debug-initramfs.cpio.lz4"
lz4 -l -12 --favor-decSpeed -q -f "$KOUT/arch/arm64/boot/Image" "$OUT/Image.lz4"

# Empty ramdisk for init_boot
E=$(mktemp -d); trap 'rm -rf "$E"' EXIT
(cd "$E" && mkdir -p empty && find empty | cpio -o -H newc -R 0:0 --quiet) | lz4 -l -q > "$OUT/empty.cpio.lz4"

# Stock vendor_boot pieces (DTB etc.)
VB=$OUT/stock-vendor_boot
rm -rf "$VB"; unpack_bootimg --boot_img "$STOCK_VB" --out "$VB" >/dev/null

mkbootimg --header_version 4 --kernel "$OUT/Image.lz4" \
    --os_version 16.0.0 --os_patch_level 2026-04 --output "$OUT/debug-boot.img"
mkbootimg --header_version 4 --ramdisk "$OUT/empty.cpio.lz4" \
    --os_version 16.0.0 --os_patch_level 2026-04 --output "$OUT/debug-init_boot.img"
# Same layout as stock vendor_boot (unpack_bootimg --format=mkbootimg), our ramdisk instead.
# The -shell variant adds tetris.debug=1 so the initramfs never switches to the rootfs.
mkvendorboot() { # mkvendorboot <output> <extra cmdline>
    mkbootimg --header_version 4 --pagesize 0x00001000 --base 0x00000000 \
        --kernel_offset 0x40000000 --ramdisk_offset 0x66f00000 \
        --tags_offset 0x47c80000 --dtb_offset 0x0000000047c80000 \
        --vendor_cmdline "bootopt=64S3,32N2,64N2 log_buf_len=1M ignore_loglevel printk.devkmsg=on $2" \
        --board '' --dtb "$VB/dtb" \
        --ramdisk_type 1 --ramdisk_name '' --vendor_ramdisk_fragment "$OUT/debug-initramfs.cpio.lz4" \
        --vendor_boot "$1"
}
mkvendorboot "$OUT/debug-vendor_boot.img" ""
mkvendorboot "$OUT/debug-vendor_boot-shell.img" "tetris.debug=1"
ls -la "$OUT"/debug-{boot,init_boot,vendor_boot,vendor_boot-shell}.img
