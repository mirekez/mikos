#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
multicore=()
tap_socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
wall_timeout="${TRIBE_INTERACTIVE_PROCESS_TIMEOUT:-1800}"

if [[ "${1:-}" == "--multicore" ]]; then
  multicore+=(--multicore)
  shift
fi
if [[ $# -ne 0 ]]; then
  echo "usage: $0 [--multicore]" >&2
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
printf '%s\n' \
  'exec 10>&1; echo MIKOS_FD10_CHILD | cat >&10; echo MIKOS_FD10_PARENT >&10' \
  "printf 'MIKOS_THREE_STAGE\\n' | cat | cat" \
  "A=parent; export A; sh -c 'A=child; test \"\$A\" = child'; echo MIKOS_ADDR_\$A" \
  "sh -c 'exit 7'; echo MIKOS_WAIT_\$?" >&3

for marker in MIKOS_FD10_CHILD MIKOS_FD10_PARENT MIKOS_THREE_STAGE \
              MIKOS_ADDR_parent MIKOS_WAIT_7; do
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

echo 'PASS: BusyBox fork, inherited pipes/fd10, address restore, and wait status'
