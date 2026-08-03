#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
multicore=()
tap="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
tap_socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
guest_address="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
wall_timeout="${TRIBE_INTERACTIVE_PING_TIMEOUT:-1800}"

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
if ! ip link show dev "$tap" >/dev/null 2>&1; then
  echo "missing host TAP interface: $tap" >&2
  exit 1
fi

runtime="$(mktemp -d /tmp/mikos-interactive-ping.XXXXXX)"
input="$runtime/uart.in"
log="$runtime/uart.log"
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

TRIBE_INTERACTIVE_CYCLES=1000000000 \
  "$root/tests/tribe/tribe_interactive.sh" "${multicore[@]}" \
  --tap-socket "$tap_socket" <"$input" >"$log" 2>&1 &
simulator_pid=$!

wait_for_prompt_count() {
  local required="$1"
  local deadline=$((SECONDS + wall_timeout))
  while ((SECONDS < deadline)); do
    local count
    count="$( (rg -o '/ # ' "$log" 2>/dev/null || true) | wc -l)"
    if ((count >= required)); then
      return 0
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null; then
      sed -n '1,320p' "$log" >&2
      echo "interactive Tribe exited before prompt $required" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,320p' "$log" >&2
  echo "timed out waiting for interactive Tribe prompt $required" >&2
  return 1
}

wait_for_prompt_count 1
printf 'ifconfig eth0 %s netmask 255.255.255.0 up\n' "$guest_address" >&3
wait_for_prompt_count 2

ping -I "$tap" -c 4 -W 10 "$guest_address"

printf 'exit\n' >&3
wait "$simulator_pid"
simulator_pid=""

for marker in 'MIKOS:NET_IP 10.0.2.15' 'MIKOS:BUSYBOX_EXIT 0' \
              'MIKOS:EXIT 0'; do
  if ! rg -q "${marker}" "$log"; then
    sed -n '1,320p' "$log" >&2
    echo "missing marker: $marker" >&2
    exit 1
  fi
done

echo "PASS: BusyBox ifconfig and host ping through $tap"
