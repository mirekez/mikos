#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 IMAGE DEBUGFS" >&2
  exit 2
fi

image="$1"
debugfs="$2"

check_mode_owner() {
  local path="$1"
  local mode="$2"
  local details
  details="$($debugfs -R "stat $path" "$image" 2>/dev/null)"
  rg -q "Type: regular +Mode:  $mode" <<<"$details"
  rg -q 'User: +0 +Group: +0' <<<"$details"
}

check_mode_owner /sbin/init 0755
check_mode_owner /bin/netstat 0755
check_mode_owner /usr/sbin/dropbear 0755
check_mode_owner /etc/init.d/rcS 0755
check_mode_owner /root/.ssh/authorized_keys 0600

inittab="$($debugfs -R 'cat /etc/inittab' "$image" 2>/dev/null)"
rg -q '^::sysinit:/etc/init\.d/rcS$' <<<"$inittab"

startup="$($debugfs -R 'cat /etc/init.d/rcS' "$image" 2>/dev/null)"
rg -q '^if /usr/sbin/dropbear -R -s -E -p 22; then$' <<<"$startup"
rg -q '^  echo "MIKOS_SSH_READY 192\.168\.76\.2:22"$' <<<"$startup"
