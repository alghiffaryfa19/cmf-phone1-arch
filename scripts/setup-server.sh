#!/usr/bin/env bash
# Turn an installed phone into the server profile, keeping userdata (run on the PC, phone
# on USB and booted into Arch). Needs the server kernel (kernel/linux.config, Docker
# options) on boot_b/vendor_boot_b: scripts/build-kernel.sh, build-debug-boot.sh,
# flash-debug-b.sh. Needs internet on the phone (WLAN, or `sudo scripts/host-nat.sh up`).
#
# Installs Docker, Avahi (tetris.local) and zram swap, the server overlay
# (rootfs/overlay-server) and tetris-display-off, then `tetris-mode server` and a reboot.
# Back to Phosh: `sudo tetris-mode phone && sudo reboot` on the phone.
#
#   setup-server.sh [--with-modem]      # --with-modem: keep mobile data as uplink
#   SUDO_PASS=...  if alarm's password is no longer "alarm"
set -euo pipefail
P=$(realpath "$(dirname "$0")/..")
SSH="$P/scripts/phone-ssh.sh"
PHONE=alarm@172.16.42.1
PASS=${SUDO_PASS:-alarm}
scp_() { scp -i "$P/out/ssh/id_ed25519" -o UserKnownHostsFile="$P/out/known_hosts" \
    -o StrictHostKeyChecking=accept-new "$@"; }
sudo_() { "$SSH" "echo '$PASS' | sudo -S -p '' sh -c '$1'"; }

echo "== kernel"
if ! "$SSH" 'zcat /proc/config.gz | grep -qx CONFIG_NF_TABLES=y'; then
    echo "!! the phone runs a kernel without the Docker options; flash the server kernel first" >&2
    exit 1
fi

echo "== packages"
sudo_ 'pacman -Sy --noconfirm --needed docker docker-compose docker-buildx avahi nss-mdns zram-generator'

echo "== overlay"
tar -C "$P/rootfs/overlay-server" --owner=0 --group=0 -cf - . | "$SSH" 'cat > /tmp/overlay-server.tar'
scp_ "$P/rootfs/src/tetris-display-off.c" "$PHONE:/tmp/"
sudo_ 'tar -C / --no-overwrite-dir -xf /tmp/overlay-server.tar &&
    gcc -O2 -Wall -Wextra -o /usr/local/bin/tetris-display-off /tmp/tetris-display-off.c &&
    rm /tmp/overlay-server.tar /tmp/tetris-display-off.c &&
    usermod -aG docker alarm && systemctl daemon-reload'

echo "== server profile"
sudo_ "tetris-mode server ${1:-}"

echo "== reboot"
sudo_ 'systemctl reboot' || true
cat <<EOF
Done. After the reboot (display stays off):
  ssh alarm@172.16.42.1                          (USB) or alarm@tetris.local (WLAN)
  docker -H ssh://alarm@tetris.local ps          from the PC
  The Ollama image is pulled on the first boot with internet (tetris-ai.service).
EOF
