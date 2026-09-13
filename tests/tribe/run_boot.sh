#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator_name=tribe64
if [[ "${1:-}" == --multicore ]]; then
  simulator_name=tribe64_multicore
  shift
fi
if (($#)); then
  echo "usage: $0 [--multicore]" >&2
  exit 2
fi
source "$root/tests/tribe/paths.sh"
simulator="$(tribe_build_directory "$simulator_name")/$simulator_name/$simulator_name"
kernel="$root/build/mikos-tribe-rv32.elf"
log="$root/build/tribe-boot-$simulator_name.log"
"$root/tests/kernel/inspect_kernel.sh" "$kernel"
status=0
timeout "${TRIBE_TIMEOUT:-600}" stdbuf -o0 -e0 "$simulator" --noveril \
  --program "$kernel" --elf --cycles "${TRIBE_CYCLES:-45000000}" \
  --start-mem-addr 0x80000000 --ram-size 8388608 --boot-priv m \
  --expected-output-contains MIKOS:FLAT_DEVICE_IRQ_OFF --mirror-uart \
  >"$log" 2>&1 || status=$?
if ((status)); then
  cat "$log" >&2
  exit "$status"
fi
for marker in MIKOS:BOOT MIKOS:KERNEL_CXX_OK MIKOS:FLAT_DEVICE_IRQ_OFF; do
  if ! grep -E -qx "$marker" "$log"; then
    cat "$log" >&2
    echo "FAIL: missing boot marker: $marker" >&2
    exit 1
  fi
done
echo "PASS: $simulator_name mikOS boot, C++ containers, flat memory and polling UART"
