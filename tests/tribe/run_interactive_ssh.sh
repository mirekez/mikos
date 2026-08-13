#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
multicore=()
simulator_backend=()
tap="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
guest_address="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
host_address="${TRIBE_INTERACTIVE_HOST_ADDRESS:-192.168.76.1}"
guest_port="${TRIBE_INTERACTIVE_SSH_PORT:-22}"
wall_timeout="${TRIBE_INTERACTIVE_SSH_TIMEOUT:-3600}"
dropbear_identity="${TRIBE_INTERACTIVE_DROPBEAR_IDENTITY:-$root/build/tests/busybox/dropbear-host/mikos_ssh_key.dropbear}"
early_client="${TRIBE_INTERACTIVE_SSH_EARLY_CLIENT:-1}"

if [[ "${MIKOS_TRIBE_SSH_NETNS_ENTERED:-0}" != 1 &&
      -z "${TRIBE_ETH_TAP_SOCKET:-}" ]]; then
  exec unshare -Urn env MIKOS_TRIBE_SSH_NETNS_ENTERED=1 "$0" "$@"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      multicore=(--multicore)
      ;;
    --verilator)
      simulator_backend=(--verilator)
      ;;
    -h|--help)
      echo "usage: $0 [--multicore] [--verilator]" >&2
      exit 0
      ;;
    *)
      echo "usage: $0 [--multicore] [--verilator]" >&2
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done
for required in "$dropbear_identity" \
                "$root/build/tests/busybox/rootfs.ext4" \
                "$root/build/tests/qemu/ethgig_tap"; do
  if [[ ! -f "$required" ]]; then
    echo "missing required SSH test input: $required" >&2
    exit 1
  fi
done

runtime="$(mktemp -d /tmp/mikos-interactive-ssh.XXXXXX)"
input="$runtime/uart.in"
log="$runtime/uart.log"
ssh_log="$runtime/ssh.log"
bridge_log="$runtime/bridge.log"
rootfs="$runtime/rootfs.ext4"
tap_socket="${TRIBE_ETH_TAP_SOCKET:-$runtime/tribe-ethgig.sock}"
cp --reflink=auto "$root/build/tests/busybox/rootfs.ext4" "$rootfs"
mkfifo "$input"
exec 3<>"$input"

cleanup() {
  if [[ -n "${ssh_pid:-}" ]]; then
    kill "$ssh_pid" 2>/dev/null || true
    wait "$ssh_pid" 2>/dev/null || true
  fi
  if [[ -n "${simulator_pid:-}" ]]; then
    kill "$simulator_pid" 2>/dev/null || true
    wait "$simulator_pid" 2>/dev/null || true
  fi
  if [[ -n "${bridge_pid:-}" ]]; then
    kill "$bridge_pid" 2>/dev/null || true
    wait "$bridge_pid" 2>/dev/null || true
  fi
  exec 3>&-
  rm -r "$runtime"
}
trap cleanup EXIT

if [[ ! -S "$tap_socket" ]]; then
  "$root/build/tests/qemu/ethgig_tap" --tap "$tap" \
    --address "$host_address/24" --socket "$tap_socket" \
    >"$bridge_log" 2>&1 &
  bridge_pid=$!
  for _ in $(seq 1 200); do
    [[ -S "$tap_socket" ]] && break
    if ! kill -0 "$bridge_pid" 2>/dev/null; then
      cat "$bridge_log" >&2
      echo "failed to start private Tribe TAP bridge" >&2
      exit 1
    fi
    sleep 0.02
  done
fi
if [[ ! -S "$tap_socket" ]] || ! ip link show dev "$tap" >/dev/null 2>&1; then
  cat "$bridge_log" >&2 2>/dev/null || true
  echo "Tribe TAP bridge did not become ready" >&2
  exit 1
fi
if [[ -n "${bridge_pid:-}" ]]; then
  # A private TAP needs only the test IPv4 subnet. Suppress automatic IPv6
  # multicast traffic, which otherwise competes with SSH in Tribe's bounded
  # real-time-to-simulated-time RX queue.
  sysctl -q -w "net.ipv6.conf.$tap.disable_ipv6=1" >/dev/null
fi

TRIBE_INTERACTIVE_CYCLES="${TRIBE_INTERACTIVE_SSH_CYCLES:-0}" \
  TRIBE_INTERACTIVE_SD_IMAGE="$rootfs" \
  "$root/tests/tribe/tribe_interactive.sh" "${multicore[@]}" \
  "${simulator_backend[@]}" --tap-socket "$tap_socket" \
  <"$input" >"$log" 2>&1 &
simulator_pid=$!

wait_for_log() {
  local marker="$1"
  local deadline=$((SECONDS + wall_timeout))
  while ((SECONDS < deadline)); do
    if rg -q "$marker" "$log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null; then
      sed -n '1,420p' "$log" >&2
      echo "interactive Tribe exited before marker: $marker" >&2
      return 1
    fi
    if [[ -n "${ssh_pid:-}" ]] && ! kill -0 "$ssh_pid" 2>/dev/null; then
      sed -n '1,420p' "$log" >&2
      sed -n '1,240p' "$ssh_log" >&2
      echo "SSH client exited before marker: $marker" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,420p' "$log" >&2
  echo "timed out waiting for marker: $marker" >&2
  return 1
}

start_ssh_client() {
  # A previous cycle-accurate run can leave a FAILED neighbor entry behind.
  # Let this connection perform fresh ARP resolution; a short host ping is not
  # a useful readiness gate because a single guest packet may take longer than
  # ping's wall-clock timeout.
  ip neigh flush to "$guest_address" dev "$tap" 2>/dev/null || true
  "$root/build/tests/busybox/dropbear-host/dbclient" \
    -i "$dropbear_identity" -c none -y -y -T -p "$guest_port" \
    "root@$guest_address" 'echo MIKOS_SSH_AUTH_OK' >"$ssh_log" 2>&1 &
  ssh_pid=$!
}

wait_for_log "MIKOS:TCP_LISTEN $guest_port"
if [[ "$early_client" == 1 ]]; then
  start_ssh_client
else
  wait_for_log 'MIKOS_SSH_STARTING 192\.168\.76\.2:22 pid='
  wait_for_log '/ # '
  printf 'ps\n' >&3
  wait_for_log 'dropbear([[:space:]]|$)'
  printf 'ifconfig -a\n' >&3
  wait_for_log 'inet addr:192\.168\.76\.2'
  printf 'netstat -ln\n' >&3
  wait_for_log '^tcp[[:space:]]+0[[:space:]]+0[[:space:]]+0\.0\.0\.0:22[[:space:]]+0\.0\.0\.0:\*[[:space:]]+LISTEN'
  start_ssh_client
fi

wait_for_log 'MIKOS:TCP_ACCEPT fd='
wait_for_log 'MIKOS:TCP_WRITE 26'
wait_for_log 'MIKOS:TCP_READ '
deadline=$((SECONDS + wall_timeout))
while ((SECONDS < deadline)); do
  if rg -q '^MIKOS_SSH_AUTH_OK$' "$ssh_log" 2>/dev/null; then
    wait "$ssh_pid"
    ssh_pid=""
    echo "PASS: background Dropbear completed the none-mode public-key proof and executed a remote command on $guest_address:$guest_port"
    exit 0
  fi
  if ! kill -0 "$ssh_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
wait "$ssh_pid" 2>/dev/null || true
ssh_pid=""
sed -n '1,420p' "$log" >&2
sed -n '1,240p' "$ssh_log" >&2
echo "FAIL: Dropbear listened but dbclient -c none did not complete its public-key proof and remote command" >&2
exit 1
