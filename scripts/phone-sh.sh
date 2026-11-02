#!/usr/bin/env bash
# Run a command on the phone's debug initramfs (busybox telnetd on 172.16.42.1:23).
# Usage: phone-sh.sh '<shell command>' [seconds to wait for output]
{ printf '%s; echo __DONE__; exit\n' "$1"; sleep "${2:-3}"; } | nc 172.16.42.1 23 \
    | LC_ALL=C tr -d '\r\377\373\374\375\376\001\003\030\037\042' | LC_ALL=C sed -n '/^__DONE__/q;p'
