#!/usr/bin/env bash
# Turn an installed phone into the desktop profile: the server profile plus a GNOME
# desktop over RDP (run on the PC, phone on USB and booted into Arch, internet on the
# phone). Needs the kernel of this branch (SO_PEERPIDFD backport, patches/kernel/) on
# slot b: scripts/build-kernel.sh, build-debug-boot.sh, flash-debug-b.sh.
#
# Installs GDM, gnome-remote-desktop and the GNOME session, the server and desktop
# overlays, then `tetris-mode desktop` and a reboot. The RDP password is created on the
# first boot: ~/.config/tetris-rdp-password on the phone (this script prints it).
#   setup-desktop.sh [--with-modem]
#   SUDO_PASS=...  if alarm's password is no longer "alarm"
set -euo pipefail
P=$(realpath "$(dirname "$0")/..")
SSH="$P/scripts/phone-ssh.sh"
PASS=${SUDO_PASS:-alarm}
sudo_() { "$SSH" "echo '$PASS' | sudo -S -p '' sh -c '$1'"; }

if ! "$SSH" 'python3 -c "import socket; socket.socketpair()[0].getsockopt(socket.SOL_SOCKET, 77)"' 2>/dev/null; then
    echo "!! the phone's kernel has no SO_PEERPIDFD; flash the kernel of this branch first" >&2
    exit 1
fi

# Server profile first (Docker, Avahi, zram, tetris-display-off, tetris-mode); it reboots
# the phone, the desktop part below runs once it is back
"$P/scripts/setup-server.sh" "${1:-}"
echo "== waiting for the phone"
sleep 20
for _ in $(seq 60); do "$SSH" -o ConnectTimeout=3 true 2>/dev/null && break; sleep 5; done

echo "== packages"
sudo_ 'pacman -Sy --noconfirm --needed gdm gnome-session gnome-remote-desktop xdg-desktop-portal-gnome gnome-backgrounds gnome-console nautilus gnome-text-editor gnome-system-monitor'

echo "== overlay"
tar -C "$P/rootfs/overlay-desktop" --owner=0 --group=0 -cf - . | "$SSH" 'cat > /tmp/overlay-desktop.tar'
sudo_ 'tar -C / --no-overwrite-dir -xf /tmp/overlay-desktop.tar && rm /tmp/overlay-desktop.tar &&
    systemctl daemon-reload && udevadm control --reload'

echo "== desktop profile"
sudo_ "tetris-mode desktop ${1:-}"
sudo_ 'systemctl reboot' || true
echo "== waiting for the first boot (RDP setup)"
sleep 30
for _ in $(seq 60); do "$SSH" -o ConnectTimeout=3 test -f .config/tetris-rdp-password 2>/dev/null && break; sleep 5; done
"$SSH" 'cat .config/tetris-rdp-password'
