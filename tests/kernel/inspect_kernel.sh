#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
elf="$root/build/mikos-rv32.elf"
sections="$root/build/mikos-rv32.sections.txt"
symbols="$root/build/mikos-rv32.undefined.txt"

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
