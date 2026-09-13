#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tap="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
address="${TRIBE_INTERACTIVE_HOST_ADDRESS:-192.168.76.1}"
guest="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
mac="${TRIBE_INTERACTIVE_GUEST_MAC:-02:00:00:00:00:02}"
bridge="$root/build/tests/tribe/ethgig_tap"

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
"$bridge" --tap "$tap" --socket "$socket" &
bridge_pid=$!
for _ in {1..100}; do
  [[ -S "$socket" ]] && break
  kill -0 "$bridge_pid" 2>/dev/null || { wait "$bridge_pid"; exit 1; }
  sleep 0.05
done
[[ -S "$socket" ]] || { echo "Bridge socket did not appear: $socket" >&2; exit 1; }
chown "${SUDO_UID:-$UID}:${SUDO_GID:-$(id -g)}" "$socket"
chmod 0600 "$socket"
echo "cpphdl TAP bridge ready: $tap $address/24, $socket (Ctrl+C to stop)"
wait "$bridge_pid"
