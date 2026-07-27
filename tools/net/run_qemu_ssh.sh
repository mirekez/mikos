#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"

if [[ "${MIKOS_NETNS_ENTERED:-0}" != 1 ]]; then
  exec unshare -Urn env MIKOS_NETNS_ENTERED=1 "$0" "$@"
fi

qemu="$root/build/qemu/qemu-system-riscv32"
kernel="$root/build/linux-qemu-out/arch/riscv/boot/Image"
initramfs="$root/build/mikos-ssh-initramfs.cpio"
bridge="$root/build/ethgig_tap"
identity="$root/build/mikos_ssh_key"
top_log="$root/build/qemu-ssh-top.log"
qemu_log="$root/build/qemu-ssh.log"
runtime="$(mktemp -d /tmp/mikos-ssh.XXXXXX)"
bridge_socket="$runtime/bridge.sock"
qemu_socket="$runtime/qemu.sock"

for required in "$qemu" "$kernel" "$initramfs" "$bridge" "$identity"; do
  if [[ ! -e "$required" ]]; then
    echo "missing required build input: $required" >&2
    exit 1
  fi
done

cleanup() {
  if [[ -n "${qemu_pid:-}" ]]; then
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
  fi
  if [[ -n "${bridge_pid:-}" ]]; then
    kill "$bridge_pid" 2>/dev/null || true
    wait "$bridge_pid" 2>/dev/null || true
  fi
  rm -r "$runtime"
}
trap cleanup EXIT

"$bridge" --tap tap-mikos --address 192.168.76.1/24 \
  --socket "$bridge_socket" --peer "$qemu_socket" \
  >"$root/build/qemu-ssh-bridge.log" 2>&1 &
bridge_pid=$!

for _ in $(seq 1 100); do
  [[ -S "$bridge_socket" ]] && break
  kill -0 "$bridge_pid" 2>/dev/null || {
    cat "$root/build/qemu-ssh-bridge.log" >&2
    exit 1
  }
  sleep 0.02
done

"$qemu" -machine virt -m 32M -nographic -monitor none -no-reboot \
  -kernel "$kernel" -initrd "$initramfs" \
  -append 'console=ttyS0 rdinit=/init' \
  -netdev dgram,id=net0,local.type=unix,local.path="$qemu_socket",remote.type=unix,remote.path="$bridge_socket" \
  -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  >"$qemu_log" 2>&1 &
qemu_pid=$!

ssh_options=(
  -i "$identity"
  -o BatchMode=yes
  -o ConnectTimeout=1
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o LogLevel=ERROR
)

connected=0
for _ in $(seq 1 240); do
  if ssh "${ssh_options[@]}" root@192.168.76.2 true 2>/dev/null; then
    connected=1
    break
  fi
  kill -0 "$qemu_pid" 2>/dev/null || break
  sleep 0.1
done

if [[ "$connected" != 1 ]]; then
  sed -n '1,320p' "$qemu_log" >&2
  cat "$root/build/qemu-ssh-bridge.log" >&2
  echo "FAIL: SSH did not become ready" >&2
  exit 1
fi

ssh "${ssh_options[@]}" root@192.168.76.2 \
  'COLUMNS=160 top -b -n 1' | tee "$top_log"

ssh "${ssh_options[@]}" root@192.168.76.2 'poweroff -f' \
  >/dev/null 2>&1 &
poweroff_pid=$!

# A kernel halt does not always make this QEMU build exit. Give it a moment,
# then stop the emulator explicitly so automated runs cannot hang forever.
for _ in $(seq 1 100); do
  kill -0 "$qemu_pid" 2>/dev/null || break
  sleep 0.02
done
if kill -0 "$qemu_pid" 2>/dev/null; then
  kill "$qemu_pid"
fi
wait "$qemu_pid" || true
qemu_pid=""
if kill -0 "$poweroff_pid" 2>/dev/null; then
  kill "$poweroff_pid" 2>/dev/null || true
fi
wait "$poweroff_pid" 2>/dev/null || true

echo "PASS: top captured over SSH in $top_log"
