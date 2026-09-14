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
  local type="${3:-regular}"
  local details
  details="$($debugfs -R "stat $path" "$image" 2>/dev/null)"
  if ! grep -E -q "Type: $type +Mode:  $mode" <<<"$details" ||
     ! grep -E -q 'User: +0 +Group: +0' <<<"$details"; then
    echo "rootfs: $path must be a root-owned $type with mode $mode" >&2
    echo "$details" >&2
    return 1
  fi
}

check_mode_owner /root 0700 directory
check_mode_owner /root/.ssh 0700 directory
check_mode_owner /sbin/init 0755
check_mode_owner /bin/netstat 0755
check_mode_owner /usr/sbin/dropbear 0755
check_mode_owner /etc/init.d/rcS 0755
check_mode_owner /etc/dropbear/dropbear_ed25519_host_key 0600
check_mode_owner /root/.ssh/authorized_keys 0600

inittab="$($debugfs -R 'cat /etc/inittab' "$image" 2>/dev/null)"
grep -E -q '^::sysinit:/etc/init\.d/rcS$' <<<"$inittab"

startup="$($debugfs -R 'cat /etc/init.d/rcS' "$image" 2>/dev/null)"
grep -E -q '^/usr/sbin/dropbear -s -F \\$' <<<"$startup"
grep -E -q '^  -r /etc/dropbear/dropbear_ed25519_host_key -p 22 &$' \
  <<<"$startup"
grep -E -q '^echo "MIKOS_SSH_STARTING 192.168.76.2:22 pid=\$!"$' \
  <<<"$startup"
