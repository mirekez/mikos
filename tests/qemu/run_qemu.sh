#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
log="$root/build/qemu-test.log"
qemu="$root/build/qemu/qemu-system-riscv32"
kernel="$root/build/mikos-rv32.elf"
rootfs="$root/build/tests/busybox/rootfs.ext4"
writable_rootfs="$root/build/qemu-test-rootfs.ext4"

cp --reflink=auto "$rootfs" "$writable_rootfs"

set +e
timeout 30 "$qemu" -machine virt -m 64M -bios none -nographic \
  -monitor none -serial stdio -no-reboot -kernel "$kernel" \
  -global virtio-mmio.force-legacy=false \
  -drive if=none,format=raw,readonly=off,id=mikos-root,file="$writable_rootfs" \
  -device virtio-blk-device,drive=mikos-root >"$log" 2>&1
status=$?
set -e

if [[ $status -ne 0 ]]; then
  sed -n '1,240p' "$log"
  echo "FAIL: QEMU status $status" >&2
  exit 1
fi

if ! grep -E -q '^MIKOS:KERNEL_CXX_OK$' "$log" ||
   ! grep -E -q '^MIKOS:EXT4_ROOT_OK$' "$log" ||
   ! grep -E -q '^MIKOS_BUSYBOX_OK$' "$log" ||
   [[ $(grep -E -c '^MIKOS_WRITE_OK$' "$log") -ne 2 ]]; then
  sed -n '1,240p' "$log"
  echo "FAIL: BusyBox marker missing" >&2
  exit 1
fi

if ! /usr/sbin/e2fsck -fn "$writable_rootfs" >"$root/build/qemu-e2fsck.log" 2>&1; then
  cat "$root/build/qemu-e2fsck.log"
  echo "FAIL: writable BusyBox run left an inconsistent ext4 image" >&2
  exit 1
fi

if ! grep -E -q '^MIKOS:BUSYBOX_EXIT 0$' "$log" ||
   ! grep -E -q '^MIKOS:STRESS_NG_ENTRY$' "$log" ||
   ! grep -E -q '^MIKOS:STRESS_NG_PASS$' "$log"; then
  sed -n '1,320p' "$log"
  echo "FAIL: bundled stress-ng CPU verification did not pass" >&2
  exit 1
fi

if ! grep -E -q '^stress-ng: metrc: .*cpu +4 ' "$log" ||
   ! grep -E -q '^stress-ng: info: .*passed: 1: cpu \(1\)$' "$log" ||
   ! grep -E -q '^stress-ng: info: .*successful run completed' "$log"; then
  sed -n '1,320p' "$log"
  echo "FAIL: stress-ng did not report the verified CPU workload" >&2
  exit 1
fi

if ! grep -E -q '^MIKOS:FLAT_DEVICE_IRQ_OFF$' "$log" ||
   ! grep -E -q '^MIKOS:PMP_ON$' "$log"; then
  sed -n '1,240p' "$log"
  echo "FAIL: platform invariants not confirmed" >&2
  exit 1
fi

if ! grep -E -q '^MIKOS:SCHED_TIMER_ON$' "$log" ||
   ! grep -E -q '^MIKOS:UNCOOPERATIVE_ENTRY$' "$log" ||
   ! grep -E -q '^MIKOS:PREEMPTIONS [1-9][0-9]*$' "$log" ||
   ! grep -E -q '^MIKOS:TIMER_CONTRACT_VIOLATIONS 0$' "$log"; then
  sed -n '1,240p' "$log"
  echo "FAIL: uncooperative task was not safely preempted" >&2
  exit 1
fi

if grep -E -q '^MIKOS:TRAP ' "$log"; then
  sed -n '1,240p' "$log"
  echo "FAIL: unexpected trap" >&2
  exit 1
fi

if ! grep -E -q '^MIKOS:EXIT 0$' "$log"; then
  sed -n '1,240p' "$log"
  echo "FAIL: bundled workloads did not exit successfully" >&2
  exit 1
fi

echo "PASS: RV32 writable ext4 BusyBox mutation, fsck, and stress-ng verification"
