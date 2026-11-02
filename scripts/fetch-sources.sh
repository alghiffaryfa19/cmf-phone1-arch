#!/usr/bin/env bash
# Shallow-clone Nothing's kernel sources for CMF Phone 1 (Tetris) into src/.
set -euo pipefail
S=$(dirname "$(realpath "$0")")/../src
B=mt6878/Tetris/16b
mkdir -p "$S"
clone() { # clone <repo> <dir> [branch]
    [[ -d $S/$2/.git ]] && { echo "$2 exists, skipping"; return; }
    git clone -q --depth 1 ${3:+-b "$3"} "https://github.com/NothingOSS/$1" "$S/$2"
}
clone android_kernel_6.1_nothing_mt6878                kernel-6.1                 $B &
clone android_kernel_device_modules_6.1_nothing_mt6878 kernel_device_modules-6.1  $B &
clone android_kernel_modules_nothing_mt6878            vendor/mediatek/kernel_modules $B &
clone android_kernel_build_nothing_mt6878              build &
wait

# Our kernel patches (idempotent: skipped when already applied)
P=$(dirname "$(realpath "$0")")/../patches
apply() { # apply <tree> <patch-dir>
    local p
    for p in "$2"/*.patch; do
        if git -C "$1" apply --check -R "$p" 2>/dev/null; then
            echo "already applied: ${p##*/}"
        else
            git -C "$1" apply "$p" && echo "applied: ${p##*/}"
        fi
    done
}
apply "$S/kernel_device_modules-6.1" "$P/kernel_device_modules"
apply "$S/vendor/mediatek/kernel_modules" "$P/kernel_modules"
