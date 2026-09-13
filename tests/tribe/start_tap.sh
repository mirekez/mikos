#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tap="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
address="${TRIBE_INTERACTIVE_HOST_ADDRESS:-192.168.76.1}"
guest="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
mac="${TRIBE_INTERACTIVE_GUEST_MAC:-02:00:00:00:00:02}"
bridge="$root/build/tests/tribe/ethgig_tap"
background=0
if [[ "${1:-}" == --background ]]; then
  background=1
  shift
fi
if (($#)); then
  echo "usage: $0 [--background]" >&2
  exit 2
fi

if [[ ! -x "$bridge" ]]; then
  echo 'Build the cpphdl bridge first: make tribe-tap' >&2
  exit 1
fi
if ((EUID != 0)); then
  echo "TAP setup requires CAP_NET_ADMIN. Run: sudo bash $root/tests/tribe/start_tap.sh" >&2
  exit 1
fi
if [[ -e "$socket" ]]; then
  echo "Socket already exists: $socket; stop its owner before starting another bridge." >&2
  exit 1
fi
if ! ip link show dev "$tap" >/dev/null 2>&1; then
  ip tuntap add dev "$tap" mode tap user "${SUDO_UID:-$UID}"
fi
ip address replace "$address/24" dev "$tap"
ip link set dev "$tap" up
ip neigh replace "$guest" lladdr "$mac" nud permanent dev "$tap"
bridge_pid=""
cleanup() {
  if [[ -n "$bridge_pid" ]]; then
    kill "$bridge_pid" 2>/dev/null || true
    wait "$bridge_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM
if ((background)); then
  log="$root/build/tests/tribe/tap.log"
  # Detach from the terminal so Ctrl+C in the simulator leaves the bridge up.
  setsid "$bridge" --tap "$tap" --socket "$socket" >"$log" 2>&1 < /dev/null &
else
  "$bridge" --tap "$tap" --socket "$socket" &
fi
bridge_pid=$!
for _ in {1..100}; do
  [[ -S "$socket" ]] && break
  if ! kill -0 "$bridge_pid" 2>/dev/null; then
    wait "$bridge_pid" || true
    if ((background)); then cat "$log" >&2; fi
    echo 'TAP bridge exited before its socket was ready.' >&2
    exit 1
  fi
  sleep 0.05
done
[[ -S "$socket" ]] || { echo "Bridge socket did not appear: $socket" >&2; exit 1; }
chown "${SUDO_UID:-$UID}:${SUDO_GID:-$(id -g)}" "$socket"
chmod 0600 "$socket"
echo "cpphdl TAP bridge ready: $tap $address/24, $socket"
if ((background)); then
  echo "Guest: $guest/24; permanent neighbor: $mac. No gateway is needed between them."
  echo "Log: $log"
  echo "Stop the background bridge: sudo kill -- $bridge_pid"
  bridge_pid=""
else
  echo 'Leave this terminal running; Ctrl+C stops the bridge.'
  wait "$bridge_pid"
fi
