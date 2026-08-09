#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator_name="tribe64"
kernel="$root/build/mikos-tribe-interactive-rv32.elf"
kernel_target="tribe-interactive-kernel"
cycles="${TRIBE_INTERACTIVE_CYCLES:-0}"
tap_socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
tap_name="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
host_address="${TRIBE_INTERACTIVE_HOST_ADDRESS:-192.168.76.1}"
guest_address="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
use_verilator="${TRIBE_INTERACTIVE_USE_VERILATOR:-0}"
verilator_cores=1
test_mode=""

usage() {
  echo "usage: $0 [--multicore] [--verilator] [--tap-socket PATH] [--test ping|tcp|process|ssh]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      simulator_name="tribe64_multicore"
      kernel="$root/build/mikos-tribe-interactive-multicore-rv32.elf"
      kernel_target="tribe-interactive-multicore-kernel"
      verilator_cores=4
      ;;
    --verilator)
      use_verilator=1
      ;;
    --test)
      if [[ $# -lt 2 ]]; then
        usage
        echo "--test requires ping, tcp, process, or ssh" >&2
        exit 2
      fi
      test_mode="$2"
      shift
      ;;
    --tap-socket)
      if [[ $# -lt 2 ]]; then
        usage
        echo "--tap-socket requires a path" >&2
        exit 2
      fi
      tap_socket="$2"
      shift
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

if [[ "$use_verilator" != 0 && "$use_verilator" != 1 ]]; then
  echo "TRIBE_INTERACTIVE_USE_VERILATOR must be 0 or 1" >&2
  exit 2
fi

if [[ -n "$test_mode" ]]; then
  case "$test_mode" in
    ping|tcp|process|ssh)
      ;;
    *)
      usage
      echo "unknown interactive test: $test_mode" >&2
      exit 2
      ;;
  esac
  test_arguments=()
  if [[ "$simulator_name" == "tribe64_multicore" ]]; then
    test_arguments+=(--multicore)
  fi
  exec env TRIBE_INTERACTIVE_USE_VERILATOR="$use_verilator" \
    "$root/tests/tribe/run_interactive_${test_mode}.sh" \
    "${test_arguments[@]}"
fi

if [[ ! -S "$tap_socket" ]]; then
  echo "Tribe TAP bridge socket is missing: $tap_socket" >&2
  echo "Start ethgig_tap for tap-tribe or pass --tap-socket PATH." >&2
  exit 1
fi
if ! ip link show dev "$tap_name" >/dev/null 2>&1; then
  echo "Host TAP interface is missing: $tap_name" >&2
  echo "The socket exists, but SSH needs a TAP bridge for 192.168.76.0/24." >&2
  exit 1
fi
if ! ip -4 -o address show dev "$tap_name" | \
     rg -q "[[:space:]]${host_address}/24([[:space:]]|$)"; then
  echo "Host TAP $tap_name does not own $host_address/24." >&2
  echo "Configure the bridge address before starting MikOS." >&2
  exit 1
fi

simulator="$root/build/tests/tribe/cpphdl-build/$simulator_name/$simulator_name"
rootfs="${TRIBE_INTERACTIVE_SD_IMAGE:-$root/build/tests/busybox/rootfs.ext4}"

if [[ ! -x "$simulator" ]]; then
  echo "Tribe simulator is missing; preparing cpphdl..." >&2
  prepare_arguments=()
  if [[ "$simulator_name" == "tribe64_multicore" ]]; then
    prepare_arguments+=(--multicore)
  fi
  "$root/tests/tribe/prepare_cpphdl.sh" "${prepare_arguments[@]}"
fi

make -C "$root" "$kernel_target" dropbear-client

if [[ ! -t 0 ]]; then
  echo "warning: stdin is not a terminal; UART input will follow stdin" >&2
fi

echo "Starting MikOS interactive BusyBox shell." >&2
echo "Type 'exit' to stop MikOS; Ctrl+C stops the simulator; Ctrl+Z suspends it." >&2
echo "Host link: $tap_name $host_address/24; guest: $guest_address/24." >&2
echo "SSH after MIKOS_SSH_STARTING:" >&2
echo "  ssh -i $root/build/mikos_ssh_key -o ConnectTimeout=600 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@$guest_address" >&2

simulator_arguments=(--noveril)
if [[ "$use_verilator" == 1 ]]; then
  verilator_simulator="$(dirname "$simulator")/TribeTest_${verilator_cores}/obj_dir/VTribeTest"
  if [[ ! -x "$verilator_simulator" || "$simulator" -nt "$verilator_simulator" ]]; then
    echo "Building the optional Verilator model for interactive execution..." >&2
    "$simulator" 1
  fi
  if [[ ! -x "$verilator_simulator" ]]; then
    echo "Verilator model was not produced: $verilator_simulator" >&2
    exit 1
  fi
  simulator="$verilator_simulator"
  simulator_arguments=()
fi

exec "$simulator" "${simulator_arguments[@]}" \
  --program "$kernel" --elf \
  --cycles "$cycles" \
  --start-mem-addr 0x80000000 \
  --ram-size 8388608 \
  --boot-priv m \
  --sd-image "$rootfs" \
  --uart-stdin \
  --eth-tap-socket "$tap_socket" \
  --expected-output-contains 'MIKOS:EXIT 0'
