#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"

exec "$root/build/qemu/qemu-system-riscv32" \
  -machine virt -m 64M -bios none -nographic -monitor none -serial stdio \
  -no-reboot -kernel "$root/build/mikos-rv32.elf"
