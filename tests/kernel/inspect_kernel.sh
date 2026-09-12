#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
elf="${1:-$root/build/mikos-rv32.elf}"
sections="${elf%.elf}.sections.txt"
symbols="${elf%.elf}.undefined.txt"

"$root/.conda/bin/llvm-readelf" -S "$elf" >"$sections"
"$root/.conda/bin/llvm-nm" --undefined-only "$elf" >"$symbols"

if rg -q '\.(eh_frame|gcc_except_table|init_array|fini_array)' "$sections"; then
  echo "FAIL: forbidden runtime section in kernel" >&2
  exit 1
fi

if [[ -s "$symbols" ]]; then
  sed -n '1,120p' "$symbols"
  echo "FAIL: undefined kernel symbols" >&2
  exit 1
fi

echo "PASS: freestanding kernel inspection"

all_symbols="$($root/.conda/bin/llvm-nm "$elf")"
if rg -q '(__cxa_|_Unwind_|__gxx_personality|_ZTI|_ZTS|_GLOBAL__sub_I_|_Z(nw|na|dl|da))' <<<"$all_symbols"; then
  echo "FAIL: unexpected hosted C++ runtime or global allocation" >&2
  exit 1
fi
symbol_address() {
  awk -v symbol="$1" '$3 == symbol { print $1 }' <<<"$all_symbols"
}
begin="$(symbol_address __kernel_begin)"
image_end="$(symbol_address __kernel_image_end)"
resident_end="$(symbol_address __kernel_end)"
[[ -n "$begin" && -n "$image_end" && -n "$resident_end" ]]
image_bytes=$((16#$image_end - 16#$begin))
resident_bytes=$((16#$resident_end - 16#$begin))
if (( image_bytes > 500000 )); then
  echo "FAIL: kernel image exceeds 500000 bytes: $image_bytes" >&2
  exit 1
fi
echo "PASS: $(basename "$elf") image=$image_bytes/500000 bytes; resident=$resident_bytes bytes (including BSS and stack, before snapshots)"
