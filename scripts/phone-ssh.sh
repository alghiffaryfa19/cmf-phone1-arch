#!/usr/bin/env bash
# ssh into the phone's Arch system over USB networking (project key in out/ssh/).
P=$(realpath "$(dirname "$0")/..")
exec ssh -i "$P/out/ssh/id_ed25519" -o UserKnownHostsFile="$P/out/known_hosts" \
    -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 alarm@172.16.42.1 "$@"
