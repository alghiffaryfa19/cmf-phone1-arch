#!/usr/bin/env bash
# Replace the kernel modules in the installed rootfs with the ones from out/ (after a
# kernel rebuild with a changed config, e.g. switching between the main and the server
# branch). The kernel refuses stale modules (modversions), so without this the modules
# loaded from the rootfs (uleds, tetris_mdtty, ...) fail. Phone on USB, booted into Arch;
# the previous set stays as /usr/lib/modules/<release>.old.
#   SUDO_PASS=...  if alarm's password is no longer "alarm"
set -euo pipefail
P=$(realpath "$(dirname "$0")/..")
OUT=$P/out
KVER=$(cat "$OUT/kernel/include/config/kernel.release")
STRIP=$P/toolchain/clang-r487747c/bin/llvm-strip
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
MD=$T/usr/lib/modules/$KVER

mkdir -p "$MD/extra"
find "$OUT/kernel" "$OUT/kernel_device_modules-6.1" "$OUT/vendor" -name '*.ko' -print0 |
    while IFS= read -r -d '' f; do "$STRIP" --strip-debug -o "$MD/$(basename "$f")" "$f"; done
for m in tetris-mdtty/tetris_mdtty tetris-torch/uleds tetris-novtswitch/tetris_novtswitch; do
    [[ -f $P/rootfs/src/$m.ko ]] && "$STRIP" --strip-debug -o "$MD/extra/${m#*/}.ko" "$P/rootfs/src/$m.ko"
done
ln -s usr/lib "$T/lib"
depmod -b "$T" -F "$OUT/kernel/System.map" "$KVER" 2>&1 | grep -vE 'needs unknown symbol|could not open modules' || true
echo "$(ls "$MD" | grep -c '\.ko$') modules for $KVER"

tar -C "$T/usr/lib/modules" --owner=0 --group=0 -czf - "$KVER" |
    "$P/scripts/phone-ssh.sh" "cat > /tmp/modules.tgz"
"$P/scripts/phone-ssh.sh" "echo '${SUDO_PASS:-alarm}' | sudo -S -p '' sh -c '
    cd /usr/lib/modules && rm -rf \"$KVER.old\" && mv \"$KVER\" \"$KVER.old\" &&
    tar -xzf /tmp/modules.tgz && rm /tmp/modules.tgz'"
echo "installed; reboot the phone to load them"
