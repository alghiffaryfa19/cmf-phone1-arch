#!/usr/bin/env bash
# Connect to the phone's GNOME desktop (desktop profile) in full HD. The phone's GNOME
# takes the resolution the client asks for; FreeRDP's default would be 1024x768, and
# /dynamic-resolution follows the window size instead.
#   desktop-rdp.sh [host] [extra xfreerdp3 options, e.g. /f for full screen]
# host: 172.16.42.1 (USB, default), tetris.local or the WLAN address
set -euo pipefail
P=$(realpath "$(dirname "$0")/..")
HOST=${1:-172.16.42.1}; shift || true
# The RDP password lives on the phone (tetris-rdp-setup); read it over USB ssh
PW=$("$P/scripts/phone-ssh.sh" "awk '{print \$NF}' .config/tetris-rdp-password")
exec xfreerdp3 /v:"$HOST" /u:alarm /p:"$PW" /size:1920x1080 /gfx:AVC444 /cert:tofu \
    +clipboard /title:"CMF Phone 1 – GNOME" "$@"
