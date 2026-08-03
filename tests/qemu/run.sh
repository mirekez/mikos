#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
rootfs="$root/build/tests/busybox/rootfs.ext4"

exec "$root/build/qemu/qemu-system-riscv32" \
  -machine virt -m 64M -bios none -nographic -monitor none -serial stdio \
  -no-reboot -kernel "$root/build/mikos-rv32.elf" \
  -global virtio-mmio.force-legacy=false \
  -drive if=none,format=raw,readonly=off,id=mikos-root,file="$rootfs" \
  -device virtio-blk-device,drive=mikos-root
