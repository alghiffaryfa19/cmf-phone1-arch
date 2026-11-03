#!/usr/bin/env bash
# Second stage, run on the PC after the first boot of the Arch rootfs (phone on USB).
# Everything that needs the phone itself to build:
#   1. Halium 16 GSI system image for the Android container (GPU, modem, camera HALs)
#   2. libhybris (+ our patches)            -> /opt/libhybris
#   3. wlroots0.20 with our hwcomposer backend (GPU Phosh)
#   4. ModemManager plugin for the MediaTek modem
#   5. camera app
#   6. GPS client (tetris-gnss)
#   7. fingerprint client (tetris-fp)
# then enables GPU Phosh and reboots. Takes a while (libhybris and wlroots compile on
# the phone) and needs internet on the phone (WLAN, or `sudo scripts/host-nat.sh up`).
#
# Usage: setup-phone.sh <halium-system.img>
#   The image: LuneOS halium_arm64 GSI (webOS-ports/halium-images, release
#   halium-luneos-20261005, halium_arm64 20261017-1); unpack it to the raw system.img.
set -euo pipefail
P=$(realpath "$(dirname "$0")/..")
IMG=${1:?usage: setup-phone.sh <halium-system.img>}
SSH="$P/scripts/phone-ssh.sh"
scp_() { scp -i "$P/out/ssh/id_ed25519" -o UserKnownHostsFile="$P/out/known_hosts" \
    -o StrictHostKeyChecking=accept-new "$@"; }
PHONE=alarm@172.16.42.1

echo "== Halium system image"
scp_ "$IMG" "$PHONE:system.img"
"$SSH" 'echo alarm | sudo -S -p "" install -Dm644 system.img /var/lib/lxc/android/system.img && rm system.img'

echo "== sources and patches"
"$SSH" 'rm -rf ~/libhybris-patches ~/mm-plugin-tetris ~/tetris-camera-app ~/tetris-gnss ~/tetris-fp; mkdir -p ~/libhybris-patches'
scp_ "$P"/pkgbuilds/*.sh "$P/patches/wlroots/0002-hwcomposer-backend-hybris-allocator.patch" "$PHONE:"
scp_ "$P"/patches/libhybris/*.patch "$PHONE:libhybris-patches/"
scp_ -r "$P/patches/modemmanager/tetris" "$PHONE:mm-plugin-tetris"
scp_ -r "$P/rootfs/src/tetris-camera-app" "$PHONE:tetris-camera-app"
scp_ -r "$P/rootfs/src/tetris-gnss" "$PHONE:tetris-gnss"
scp_ -r "$P/rootfs/src/tetris-fp" "$PHONE:tetris-fp"

echo "== libhybris"
"$SSH" '~/build-libhybris.sh'
echo "== wlroots (hwcomposer backend)"
"$SSH" '~/build-wlroots-phoc.sh'
echo "== ModemManager plugin"
"$SSH" '~/build-mm-plugin-tetris.sh'
echo "== camera app"
"$SSH" 'make -C ~/tetris-camera-app && echo alarm | sudo -S -p "" make -C ~/tetris-camera-app install'

echo "== GPS"
"$SSH" 'make -C ~/tetris-gnss && echo alarm | sudo -S -p "" sh -c "make -C ~/tetris-gnss install && systemctl enable tetris-gnss"'

echo "== fingerprint"
"$SSH" 'make -C ~/tetris-fp && echo alarm | sudo -S -p "" sh -c "make -C ~/tetris-fp install && systemctl enable tetris-fp"'

echo "== GPU Phosh on, reboot"
"$SSH" 'echo alarm | sudo -S -p "" sh -c "touch /etc/tetris-android/gpu-shell; systemctl reboot"' || true
echo "Done. After the reboot Phosh starts on the GPU (falls back to software if that fails)."
