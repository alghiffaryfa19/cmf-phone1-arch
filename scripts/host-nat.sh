#!/usr/bin/env bash
# Share the PC's internet with the phone over USB NCM (phone 172.16.42.1, PC 172.16.42.2).
# Run as root: sudo scripts/host-nat.sh [up|down]   (not persistent; gone after reboot)
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root"; exit 1; }
NET=172.16.42.0/24
WAN=$(ip route show default | awk '{print $5; exit}')
case ${1:-up} in
up)
    sysctl -qw net.ipv4.ip_forward=1
    iptables -t nat -C POSTROUTING -s $NET -o "$WAN" -j MASQUERADE 2>/dev/null ||
        iptables -t nat -A POSTROUTING -s $NET -o "$WAN" -j MASQUERADE
    # Docker sets FORWARD policy DROP, so allow the phone's traffic explicitly
    iptables -C FORWARD -s $NET -j ACCEPT 2>/dev/null || iptables -I FORWARD -s $NET -j ACCEPT
    iptables -C FORWARD -d $NET -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
        iptables -I FORWARD -d $NET -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    echo "NAT up: $NET -> $WAN"
    ;;
down)
    iptables -t nat -D POSTROUTING -s $NET -o "$WAN" -j MASQUERADE 2>/dev/null || true
    iptables -D FORWARD -s $NET -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -d $NET -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    echo "NAT down"
    ;;
esac
