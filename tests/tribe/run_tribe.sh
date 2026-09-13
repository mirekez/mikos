#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator_name="tribe64"
kernel_only=0

usage() {
  echo "usage: $0 [--multicore] [--kernel-only]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --kernel-only)
      kernel_only=1
      ;;
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

source "$root/tests/tribe/paths.sh"
simulator="$(tribe_build_directory "$simulator_name")/$simulator_name/$simulator_name"
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
required_inputs=("$simulator" "$kernel" "$peer")
media_arguments=()
expected_output='MIKOS:EXIT 0'
if ((kernel_only)); then
  log="$root/build/tribe-kernel-$simulator_name.log"
  peer_log="$root/build/tribe-kernel-$simulator_name-peer.log"
  expected_output='MIKOS:ICMP_ECHO_REPLY'
else
  required_inputs+=("$sd_image")
  media_arguments=(--sd-image "$sd_image")
fi

cleanup() {
  if [[ -n "${peer_pid:-}" ]]; then
    kill "$peer_pid" 2>/dev/null || true
    wait "$peer_pid" 2>/dev/null || true
  fi
  rm -r "$runtime"
}
trap cleanup EXIT

for required in "${required_inputs[@]}"; do
  if [[ ! -e "$required" ]]; then
    echo "missing test input: $required" >&2
    exit 1
  fi
done

"$root/tests/kernel/inspect_kernel.sh" "$kernel"

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
timeout "$wall_timeout" stdbuf -o0 -e0 "$simulator" --noveril \
  --program "$kernel" --elf \
  --cycles "$cycles" \
  --start-mem-addr 0x80000000 \
  --ram-size 8388608 \
  --boot-priv m \
  "${media_arguments[@]}" \
  --eth-tap-socket "$media_socket" \
  --expected-output-contains "$expected_output" \
  --mirror-uart >"$log" 2>&1
simulator_status=$?
set -e

# Give the peer time to consume the final datagram and publish its verdict.
# Simulator exit alone is not proof that the host validated the reply.
if [[ $simulator_status -eq 0 ]]; then
  for _ in $(seq 1 100); do
    kill -0 "$peer_pid" 2>/dev/null || break
    sleep 0.01
  done
fi
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

if ((kernel_only)); then
  for marker in MIKOS:BOOT MIKOS:KERNEL_CXX_OK MIKOS:FLAT_DEVICE_IRQ_OFF \
      MIKOS:ARP_REPLY MIKOS:ICMP_ECHO_REPLY; do
    if ! rg -qx "$marker" "$log"; then
      cat "$log" >&2
      echo "FAIL: missing kernel marker: $marker" >&2
      exit 1
    fi
  done
  echo "PASS: $simulator_name mikOS boot, C++ containers, flat memory, polling UART, ARP and ICMP (no rootfs attached)"
  exit 0
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
