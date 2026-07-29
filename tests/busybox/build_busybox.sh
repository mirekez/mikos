#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 6 ]]; then
  echo "usage: $0 SOURCE OUTPUT CROSS_PREFIX ADDRESS LLVM_READELF JOBS" >&2
  exit 2
fi

source_tree="$1"
output="$2"
cross_prefix="$3"
address="$4"
llvm_readelf="$5"
jobs="$6"
tool_directory="$(dirname "$cross_prefix")"

PATH="$tool_directory:$PATH" make -C "$source_tree" O="$output" \
  -j"$jobs" CONFIG_EXTRA_LDFLAGS="-Wl,-Ttext-segment=$address" busybox
"$llvm_readelf" -l "$output/busybox" >"$output/program-headers.txt"
rg -q "LOAD +0x[0-9a-f]+ +$address" "$output/program-headers.txt"
