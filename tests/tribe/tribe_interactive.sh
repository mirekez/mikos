#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator_name="tribe64"
kernel="$root/build/mikos-tribe-interactive-rv32.elf"
kernel_target="tribe-interactive-kernel"
cycles="${TRIBE_INTERACTIVE_CYCLES:-1000000000}"
tap_socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"

usage() {
  echo "usage: $0 [--multicore] [--tap-socket PATH]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      simulator_name="tribe64_multicore"
      kernel="$root/build/mikos-tribe-interactive-multicore-rv32.elf"
      kernel_target="tribe-interactive-multicore-kernel"
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

if [[ ! -S "$tap_socket" ]]; then
  echo "Tribe TAP bridge socket is missing: $tap_socket" >&2
  echo "Start ethgig_tap for tap-tribe or pass --tap-socket PATH." >&2
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

make -C "$root" "$kernel_target"

if [[ ! -t 0 ]]; then
  echo "warning: stdin is not a terminal; UART input will follow stdin" >&2
fi

echo "Starting MikOS interactive BusyBox shell." >&2
echo "Type 'exit' to stop MikOS; Ctrl+C stops the simulator; Ctrl+Z suspends it." >&2

exec "$simulator" --noveril \
  --program "$kernel" --elf \
  --cycles "$cycles" \
  --start-mem-addr 0x80000000 \
  --ram-size 8388608 \
  --boot-priv m \
  --sd-image "$rootfs" \
  --uart-stdin \
  --eth-tap-socket "$tap_socket" \
  --expected-output-contains 'MIKOS:EXIT 0'
