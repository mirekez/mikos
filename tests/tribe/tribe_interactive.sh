#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator="$root/build/tests/tribe/cpphdl-build/tribe64/tribe64"
kernel="$root/build/mikos-tribe-interactive-rv32.elf"
cycles="${TRIBE_INTERACTIVE_CYCLES:-1000000000}"

if [[ ! -x "$simulator" ]]; then
  echo "Tribe simulator is missing; preparing cpphdl..." >&2
  "$root/tests/tribe/prepare_cpphdl.sh"
fi

make -C "$root" tribe-interactive-kernel

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
  --uart-stdin \
  --expected-output-contains 'MIKOS:EXIT 0'
