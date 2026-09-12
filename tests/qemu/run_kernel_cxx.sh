#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
qemu="${QEMU_RISCV32:-$root/build/qemu/qemu-system-riscv32}"
log="$root/build/kernel-cxx-boot.log"
# No disk or network required: the real kernel runs the container test before
# mounting root. Exit 8 is the expected missing-root result.
set +e
timeout 20 "$qemu" -machine virt -m 32M -bios none -nographic \
  -monitor none -no-reboot -kernel "$root/build/mikos-rv32.elf" >"$log" 2>&1
status=$?
set -e
if [[ $status -ne 8 ]] || ! rg -q '^MIKOS:KERNEL_CXX_OK$' "$log" || \
   ! rg -q '^MIKOS:EXT4_ROOT_FAIL$' "$log"; then
  cat "$log"
  echo "FAIL: kernel container boot test (exit $status)" >&2
  exit 1
fi
echo "PASS: RV32 kernel executes vector, list, map, unordered_set, inplace_vector and releases all arena allocations"
