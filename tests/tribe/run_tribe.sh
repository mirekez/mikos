#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator_name="tribe64"

usage() {
  echo "usage: $0 [--multicore]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      simulator_name="tribe64_multicore"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

simulator="$root/build/tests/tribe/cpphdl-build/$simulator_name/$simulator_name"
kernel="$root/build/mikos-tribe-rv32.elf"
peer="$root/build/tests/tribe/net_peer"
log="$root/build/tribe-test.log"
peer_log="$root/build/tribe-net-peer.log"
cycles="${TRIBE_CYCLES:-45000000}"
default_wall_timeout=600
if [[ "$simulator_name" == "tribe64_multicore" ]]; then
  default_wall_timeout=1800
fi
wall_timeout="${TRIBE_TIMEOUT:-$default_wall_timeout}"
runtime="$(mktemp -d /tmp/mikos-tribe.XXXXXX)"
media_socket="$runtime/ethernet.sock"
sd_image="$root/build/tests/busybox/rootfs.ext4"

cleanup() {
  if [[ -n "${peer_pid:-}" ]]; then
    kill "$peer_pid" 2>/dev/null || true
    wait "$peer_pid" 2>/dev/null || true
  fi
  rm -r "$runtime"
}
trap cleanup EXIT

for required in "$simulator" "$kernel" "$peer" "$sd_image"; do
  if [[ ! -e "$required" ]]; then
    echo "missing test input: $required" >&2
    exit 1
  fi
done

"$peer" "$media_socket" >"$peer_log" 2>&1 &
peer_pid=$!
for _ in $(seq 1 500); do
  [[ -S "$media_socket" ]] && break
  kill -0 "$peer_pid" 2>/dev/null || break
  sleep 0.01
done
if [[ ! -S "$media_socket" ]]; then
  cat "$peer_log" >&2
  echo "FAIL: Tribe network peer did not become ready" >&2
  exit 1
fi

set +e
timeout "$wall_timeout" "$simulator" --noveril \
  --program "$kernel" --elf \
  --cycles "$cycles" \
  --start-mem-addr 0x80000000 \
  --ram-size 8388608 \
  --boot-priv m \
  --sd-image "$sd_image" \
  --eth-tap-socket "$media_socket" \
  --expected-output-contains 'MIKOS:EXIT 0' \
  --mirror-uart >"$log" 2>&1
simulator_status=$?
set -e

set +e
if kill -0 "$peer_pid" 2>/dev/null; then
  kill "$peer_pid" 2>/dev/null
  wait "$peer_pid"
  peer_status=1
else
  wait "$peer_pid"
  peer_status=$?
fi
set -e
peer_pid=""

if [[ $simulator_status -ne 0 || $peer_status -ne 0 ]]; then
  sed -n '1,320p' "$log" >&2
  cat "$peer_log" >&2
  echo "FAIL: Tribe simulator status $simulator_status, network peer status $peer_status" >&2
  exit 1
fi

if ! rg -q '^MIKOS:EXT4_ROOT_OK$' "$log" ||
   ! rg -q '^MIKOS:NET_IP 10\.0\.2\.15$' "$log" ||
   ! rg -q '^MIKOS:NET_MAC 02:00:00:00:00:02$' "$log" ||
   ! rg -q '^MIKOS:ARP_REPLY$' "$log" ||
   ! rg -q '^MIKOS:ICMP_ECHO_REPLY$' "$log" ||
   ! rg -q '^MIKOS:PMP_UNAVAILABLE$' "$log" ||
   ! rg -q '^MIKOS:TRIBE_POLLING$' "$log" ||
   ! rg -q '^MIKOS_BUSYBOX_OK$' "$log" ||
   ! rg -q '^MIKOS:BUSYBOX_EXIT 0$' "$log" ||
   ! rg -q '^MIKOS:EXIT 0$' "$log"; then
  sed -n '1,320p' "$log" >&2
  cat "$peer_log" >&2
  echo "FAIL: MikOS Tribe acceptance markers missing" >&2
  exit 1
fi

echo "PASS: Tribe UART, ext4-root BusyBox, ARP, and IPv4 ICMP ping"
