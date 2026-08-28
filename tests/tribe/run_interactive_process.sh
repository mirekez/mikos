#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
multicore=()
tap_socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
wall_timeout="${TRIBE_INTERACTIVE_PROCESS_TIMEOUT:-7200}"
nested_only="${TRIBE_INTERACTIVE_PROCESS_NESTED_ONLY:-0}"

if [[ "${1:-}" == "--multicore" ]]; then
  multicore+=(--multicore)
  shift
fi
if [[ $# -ne 0 ]]; then
  echo "usage: $0 [--multicore]" >&2
  exit 2
fi
if [[ "$nested_only" != 0 && "$nested_only" != 1 ]]; then
  echo "TRIBE_INTERACTIVE_PROCESS_NESTED_ONLY must be 0 or 1" >&2
  exit 2
fi
if [[ ! -S "$tap_socket" ]]; then
  echo "missing Tribe TAP bridge socket: $tap_socket" >&2
  exit 1
fi

runtime="$(mktemp -d /tmp/mikos-interactive-process.XXXXXX)"
input="$runtime/uart.in"
log="$runtime/uart.log"
rootfs="$runtime/rootfs.ext4"
cp --reflink=auto "$root/build/tests/busybox/rootfs.ext4" "$rootfs"
debugfs="${DEBUGFS:-/usr/sbin/debugfs}"
process_rcs="$root/tests/busybox/rootfs/etc/init.d/rcS.process"
if [[ ! -x "$debugfs" ]]; then
  echo "missing debugfs: $debugfs" >&2
  exit 1
fi
"$debugfs" -w -R 'rm /etc/init.d/rcS' "$rootfs" >/dev/null 2>&1
"$debugfs" -w -R "write $process_rcs /etc/init.d/rcS" \
  "$rootfs" >/dev/null 2>&1
"$debugfs" -w -R 'set_inode_field /etc/init.d/rcS mode 0100755' \
  "$rootfs" >/dev/null 2>&1
mkfifo "$input"
exec 3<>"$input"

cleanup() {
  if [[ -n "${simulator_pid:-}" ]]; then
    kill "$simulator_pid" 2>/dev/null || true
    wait "$simulator_pid" 2>/dev/null || true
  fi
  exec 3>&-
  rm -r "$runtime"
}
trap cleanup EXIT

TRIBE_INTERACTIVE_CYCLES=1000000000 TRIBE_INTERACTIVE_SD_IMAGE="$rootfs" \
  "$root/tests/tribe/tribe_interactive.sh" "${multicore[@]}" \
  --tap-socket "$tap_socket" <"$input" >"$log" 2>&1 &
simulator_pid=$!

wait_for_marker() {
  local marker="$1"
  local deadline=$((SECONDS + wall_timeout))
  while ((SECONDS < deadline)); do
    if rg -q "$marker" "$log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null; then
      sed -n '1,360p' "$log" >&2
      echo "interactive Tribe exited before marker: $marker" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,360p' "$log" >&2
  echo "timed out waiting for marker: $marker" >&2
  return 1
}

wait_for_marker '/ # '
if [[ "$nested_only" == 1 ]]; then
  commands=("sh -c 'ls /'; echo MIKOS_NESTED_LS_\$?")
  markers=('MIKOS:NESTED_CLONE_STACK depth=1' MIKOS_NESTED_LS_0)
else
  commands=(
    'exec 10>&1; echo MIKOS_FD10_CHILD | cat >&10; echo MIKOS_FD10_PARENT >&10'
    "printf 'MIKOS_THREE_STAGE\\n' | cat | cat"
    "A=parent; export A; sh -c 'A=child; test \"\$A\" = child'; echo MIKOS_ADDR_\$A"
    "sh -c 'exit 7'; echo MIKOS_WAIT_\$?"
    "sh -c 'ls /'; echo MIKOS_NESTED_LS_\$?"
  )
  markers=(MIKOS_FD10_CHILD MIKOS_FD10_PARENT MIKOS_THREE_STAGE
           MIKOS_ADDR_parent MIKOS_WAIT_7
           'MIKOS:NESTED_CLONE_STACK depth=1' MIKOS_NESTED_LS_0)
fi
printf '%s\n' "${commands[@]}" >&3

for marker in "${markers[@]}"; do
  wait_for_marker "(^|/ # )${marker}\\r?$"
done

printf 'exit\n' >&3
wait "$simulator_pid"
simulator_pid=""

for marker in 'MIKOS:BUSYBOX_EXIT 0' 'MIKOS:EXIT 0'; do
  if ! rg -q "$marker" "$log"; then
    sed -n '1,360p' "$log" >&2
    echo "missing marker: $marker" >&2
    exit 1
  fi
done

if [[ "$nested_only" == 1 ]]; then
  echo 'PASS: nested BusyBox shell fork and external ls'
else
  echo 'PASS: BusyBox fork, inherited pipes/fd10, address restore, wait status, and nested ls'
fi
