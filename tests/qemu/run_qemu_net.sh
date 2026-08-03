#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
runtime="$(mktemp -d /tmp/mikos-net.XXXXXX)"
qemu_socket="$runtime/qemu.sock"
peer_socket="$runtime/peer.sock"
log="$root/build/qemu-net-test.log"
qemu="$root/build/qemu/qemu-system-riscv32"
kernel="$root/build/mikos-rv32.elf"
rootfs="$root/build/tests/busybox/rootfs.ext4"

cleanup() {
  if [[ -n "${qemu_pid:-}" ]]; then
    kill "$qemu_pid" 2>/dev/null || true
  fi
  rm -r "$runtime"
}
trap cleanup EXIT

timeout 20 "$qemu" -machine virt -m 64M -bios none -nographic \
  -monitor none -serial stdio -no-reboot -kernel "$kernel" \
  -global virtio-mmio.force-legacy=false \
  -drive if=none,format=raw,readonly=on,id=mikos-root,file="$rootfs" \
  -device virtio-blk-device,drive=mikos-root \
  -netdev dgram,id=net0,local.type=unix,local.path="$qemu_socket",remote.type=unix,remote.path="$peer_socket" \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  >"$log" 2>&1 &
qemu_pid=$!

for _ in $(seq 1 500); do
  if [[ -S "$qemu_socket" ]] &&
     rg -q '^MIKOS:NET_MAC 52:54:00:12:34:56$' "$log"; then
    break
  fi
  sleep 0.01
done

if [[ ! -S "$qemu_socket" ]] ||
   ! rg -q '^MIKOS:NET_MAC 52:54:00:12:34:56$' "$log"; then
  sed -n '1,240p' "$log"
  echo "FAIL: QEMU network endpoint did not become ready" >&2
  exit 1
fi

"$root/build/tests/qemu/net_peer" "$peer_socket" "$qemu_socket"
wait "$qemu_pid"
qemu_pid=""

if ! rg -q '^MIKOS:EXT4_ROOT_OK$' "$log" ||
   ! rg -q '^MIKOS:NET_IP 10\.0\.2\.15$' "$log" ||
   ! rg -q '^MIKOS:NET_MAC 52:54:00:12:34:56$' "$log" ||
   ! rg -q '^MIKOS:ARP_REPLY$' "$log" ||
   ! rg -q '^MIKOS:ICMP_ECHO_REPLY$' "$log" ||
   ! rg -q '^MIKOS:EXIT 0$' "$log"; then
  sed -n '1,240p' "$log"
  echo "FAIL: guest network probe" >&2
  exit 1
fi

echo "PASS: RV32 polled virtio-net ARP and ICMP"
