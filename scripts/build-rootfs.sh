#!/usr/bin/env bash
# Build the Arch Linux ARM rootfs image for CMF Phone 1 (Tetris).  Run as root:
#   sudo scripts/build-rootfs.sh
#   sudo scripts/build-rootfs.sh --finish   # re-apply overlay/services + shrink on an existing image
#   sudo PROFILE=server scripts/build-rootfs.sh   # headless server: Docker, Ollama, no Phosh
#   sudo PROFILE=desktop scripts/build-rootfs.sh  # server + GNOME over RDP
# Output: out/rootfs.img (raw ext4) + out/rootfs.simg (sparse, for `fastboot flash userdata`).
# The image is small; the phone grows it to the full userdata partition on first boot.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (sudo $0)"; exit 1; }

P=$(realpath "$(dirname "$0")/..")
OUT=$P/out
IMG=$OUT/rootfs.img
MNT=$OUT/rootfs-mnt
TARBALL=$P/downloads/alarm/ArchLinuxARM-aarch64-latest.tar.gz
KOUT=$OUT/kernel
KVER=$(cat "$KOUT/include/config/kernel.release")
STRIP=$P/toolchain/clang-r487747c/bin/llvm-strip
OWNER=$(stat -c %u:%g "$P")
SIZE=${SIZE:-10G}
PROFILE=${PROFILE:-phone}
[[ $PROFILE == phone || $PROFILE == server || $PROFILE == desktop ]] ||
    { echo "PROFILE must be phone, server or desktop"; exit 1; }

PACKAGES=(
    # UI
    phosh phoc squeekboard phosh-mobile-settings gnome-console gnome-control-center
    gnome-text-editor nautilus adwaita-icon-theme cantarell-fonts noto-fonts noto-fonts-emoji
    # system
    networkmanager modemmanager chatty gnome-calls gnome-maps wpa_supplicant iw feedbackd gnome-keyring bluez bluez-utils
    openssh sudo vim htop bash-completion usbutils pciutils e2fsprogs
    # gcc: crt files + libgcc for the cross-compiled helpers below; the rest for the
    # on-phone builds of scripts/setup-phone.sh (libhybris, wlroots, ModemManager plugin)
    base-devel git meson
    # Halium container for the Android HALs (GPU, modem, camera)
    lxc
)
# Server profile (rootfs/overlay-server, tetris-mode): containers, mDNS
# name tetris.local, compressed swap. Ollama runs as a container (tetris-ai.service);
# Arch Linux ARM has no ollama package. The phone packages stay installed so that
# `tetris-mode phone` can switch back.
SERVER_PACKAGES=(docker docker-compose docker-buildx avahi nss-mdns zram-generator)
# Desktop profile (rootfs/overlay-desktop): headless GNOME session behind RDP
DESKTOP_PACKAGES=(gdm gnome-session gnome-remote-desktop xdg-desktop-portal-gnome gnome-backgrounds
    gnome-console nautilus gnome-text-editor gnome-system-monitor)
[[ $PROFILE != phone ]] && PACKAGES+=("${SERVER_PACKAGES[@]}")
[[ $PROFILE == desktop ]] && PACKAGES+=("${DESKTOP_PACKAGES[@]}")

cleanup() {
    mountpoint -q "$MNT" && umount -R "$MNT" || true
}
trap cleanup EXIT

FINISH=0; [[ ${1:-} == --finish ]] && FINISH=1
mkdir -p "$MNT"
if (( FINISH )); then
    [[ -f $IMG ]] || { echo "no $IMG to finish"; exit 1; }
    e2fsck -fy "$IMG" >/dev/null || true
    resize2fs "$IMG" "$SIZE" >/dev/null
    mount -o loop "$IMG" "$MNT"
else
    [[ -f $TARBALL ]] || { echo "missing $TARBALL"; exit 1; }
    rm -f "$IMG"
    truncate -s "$SIZE" "$IMG"
    mkfs.ext4 -q -L tetris-root "$IMG"
    mount -o loop "$IMG" "$MNT"

    echo "== extracting $(basename "$TARBALL")"
    bsdtar -xpf "$TARBALL" -C "$MNT"

    echo "== kernel modules ($KVER)"
    MD=$MNT/usr/lib/modules/$KVER
    mkdir -p "$MD"
    find "$OUT/kernel" "$OUT/kernel_device_modules-6.1" "$OUT/vendor" -name '*.ko' -print0 |
        while IFS= read -r -d '' f; do "$STRIP" --strip-debug -o "$MD/$(basename "$f")" "$f"; done
    # AT channel tty pair for ModemManager (built by build-kernel.sh ext)
    install -Dm644 "$P/rootfs/src/tetris-mdtty/tetris_mdtty.ko" "$MD/extra/tetris_mdtty.ko"
    # userspace LED devices for tetris-torch (CONFIG_LEDS_USER is off)
    install -Dm644 "$P/rootfs/src/tetris-torch/uleds.ko" "$MD/extra/uleds.ko"
    depmod -b "$MNT" -F "$KOUT/System.map" "$KVER"

fi

echo "== overlay"
cp -a "$P/rootfs/overlay/." "$MNT/"
[[ $PROFILE != phone ]] && cp -a "$P/rootfs/overlay-server/." "$MNT/"
[[ $PROFILE == desktop ]] && cp -a "$P/rootfs/overlay-desktop/." "$MNT/"
# stock vendor firmware (scripts/extract-vendor.sh), see firmware_class.path=/vendor/firmware
mkdir -p "$MNT/vendor"; rm -rf "$MNT/vendor/firmware"
cp -a "$OUT/vendor-extract/vendor/firmware" "$MNT/vendor/"; chown -R 0:0 "$MNT/vendor"
chown -R 0:0 "$MNT/etc/sudoers.d" "$MNT/etc/systemd" "$MNT/etc/NetworkManager" "$MNT/etc/modprobe.d" \
    "$MNT/etc/udev/rules.d" "$MNT/usr/local/bin"
if [[ $PROFILE != phone ]]; then
    chown -R 0:0 "$MNT/etc/ssh/sshd_config.d" "$MNT/etc/docker" "$MNT/etc/sysctl.d" "$MNT/usr/local/bin/tetris-mode" "$MNT/opt/tetris-server"
fi
chmod 0440 "$MNT/etc/sudoers.d/10-wheel"
chmod 0600 "$MNT/etc/NetworkManager/conf.d/"*.conf "$MNT/etc/NetworkManager/system-connections/"*.nmconnection

# The project key (out/ssh, used by scripts/phone-ssh.sh; created here if missing) and the
# host user's ssh keys, so `ssh alarm@172.16.42.1` works without a password
HOME_DIR=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
if [[ ! -f $P/out/ssh/id_ed25519 ]]; then
    install -d -m700 "$P/out/ssh"
    ssh-keygen -q -t ed25519 -N '' -C cmf-phone1-arch -f "$P/out/ssh/id_ed25519"
    [[ -n ${SUDO_USER:-} ]] && chown -R "$SUDO_USER:" "$P/out/ssh"
fi
install -d -m700 -o 1000 -g 1000 "$MNT/home/alarm/.ssh"
cat "$P/out/ssh/id_ed25519.pub" $(compgen -G "$HOME_DIR/.ssh/*.pub" || true) \
    > "$MNT/home/alarm/.ssh/authorized_keys"
chown 1000:1000 "$MNT/home/alarm/.ssh/authorized_keys"; chmod 600 "$MNT/home/alarm/.ssh/authorized_keys"

if (( ! FINISH )); then
echo "== packages (in aarch64 chroot via qemu)"
arch-chroot "$MNT" /bin/bash -euxo pipefail -c "
    pacman-key --init
    pacman-key --populate archlinuxarm
    # our kernel comes from boot_b; drop the generic one and its initramfs tooling
    pacman -Rdd --noconfirm linux-aarch64 || true
    # pacman 7 download sandbox (Landlock + setuid alpm) does not work under qemu-user
    pacman -Syu --noconfirm --needed --disable-sandbox ${PACKAGES[*]}
"
fi

echo "== tetris-bt-bridge (cross-compiled against the rootfs glibc)"
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu --sysroot="$MNT" \
    -fuse-ld=lld -O2 -Wall -Wextra -o "$MNT/usr/local/bin/tetris-bt-bridge" \
    "$P/rootfs/src/tetris-bt-bridge.c"
chown 0:0 "$MNT/usr/local/bin/tetris-bt-bridge"

echo "== tetris-mdtty-bridge (cross-compiled against the rootfs glibc)"
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu --sysroot="$MNT" \
    -fuse-ld=lld -O2 -Wall -Wextra -o "$MNT/usr/local/bin/tetris-mdtty-bridge" \
    "$P/rootfs/src/tetris-mdtty-bridge.c"
chown 0:0 "$MNT/usr/local/bin/tetris-mdtty-bridge"
echo "== tetris-vibra"
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu --sysroot="$MNT" \
    -fuse-ld=lld -O2 -Wall -Wextra \
    -o "$MNT/usr/local/bin/tetris-vibra" "$P/rootfs/src/tetris-vibra.c"
chown 0:0 "$MNT/usr/local/bin/tetris-vibra"
echo "== tetris-torch"
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu --sysroot="$MNT" \
    -fuse-ld=lld -O2 -Wall -Wextra \
    -o "$MNT/usr/local/bin/tetris-torch" "$P/rootfs/src/tetris-torch/tetris-torch.c"
chown 0:0 "$MNT/usr/local/bin/tetris-torch"
echo "== tetris-display-off"
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu --sysroot="$MNT" \
    -fuse-ld=lld -O2 -Wall -Wextra \
    -o "$MNT/usr/local/bin/tetris-display-off" "$P/rootfs/src/tetris-display-off.c"
chown 0:0 "$MNT/usr/local/bin/tetris-display-off"
echo "== tetris-sensors (needs glib's gio from the rootfs)"
"$P/toolchain/clang-r487747c/bin/clang" --target=aarch64-linux-gnu --sysroot="$MNT" \
    -fuse-ld=lld -O2 -Wall \
    $(PKG_CONFIG_SYSROOT_DIR="$MNT" PKG_CONFIG_LIBDIR="$MNT/usr/lib/pkgconfig:$MNT/usr/share/pkgconfig" \
      pkg-config --cflags --libs gio-2.0 gio-unix-2.0) -lm \
    -o "$MNT/usr/local/bin/tetris-sensors" "$P/rootfs/src/tetris-sensors/tetris-sensors.c"
chown 0:0 "$MNT/usr/local/bin/tetris-sensors"
# libmm-plugin-tetris.so needs ModemManager's source tree: built on the phone afterwards
# with pkgbuilds/build-mm-plugin-tetris.sh (+ patches/modemmanager/tetris)
# The camera app (rootfs/src/tetris-camera-app) needs /opt/libhybris, also built on the
# phone: copy the directory over, then `make && sudo make install` there.

echo "== services"
arch-chroot "$MNT" /bin/bash -euxo pipefail -c "
    # our GKI kernel has no Landlock yet, so pacman's download sandbox fails on the phone too
    grep -q '^DisableSandbox' /etc/pacman.conf || sed -i '/^\\[options\\]/a DisableSandbox' /etc/pacman.conf
    usermod -aG wheel,video,input,audio,network alarm
    systemctl enable sshd NetworkManager ModemManager systemd-networkd tetris-phosh tetris-growfs tetris-wlan bluetooth tetris-boot-success tetris-bt-bridge tetris-modem tetris-cpuidle tetris-vibra tetris-torch tetris-sensors tetris-splash-stop tetris-usb-role
    systemctl mask iio-sensor-proxy  # no IIO sensors here; tetris-sensors serves its D-Bus API
    systemctl --global enable sm.puri.Chatty-daemon  # SMS arrive while the Chats window is closed
    systemctl set-default graphical.target
    if [[ $PROFILE != phone ]]; then
        usermod -aG docker alarm
        /usr/local/bin/tetris-mode $PROFILE
    fi
    rm -rf /var/cache/pacman/pkg/*
    rm -rf /etc/systemd/system/phosh.service.d  # leftover from an earlier overlay
"
rm -f "$MNT/boot/"{Image,Image.gz,initramfs-linux*.img} 2>/dev/null || true
rm -rf "$MNT/boot/dtbs" 2>/dev/null || true

umount -R "$MNT"
trap - EXIT
e2fsck -fy "$IMG" >/dev/null || true
resize2fs -M "$IMG" >/dev/null
echo "== sparse image"
img2simg "$IMG" "$OUT/rootfs.simg"
chown "$OWNER" "$IMG" "$OUT/rootfs.simg"
ls -la "$IMG" "$OUT/rootfs.simg"
