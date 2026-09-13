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

# Conda's host flags can contain x86 ISA options and host-only include paths.
# BusyBox supplies target flags from its own configuration.
unset CFLAGS CXXFLAGS CPPFLAGS AFLAGS LDFLAGS

PATH="$tool_directory:$PATH" make -C "$source_tree" O="$output" \
  -j"$jobs" CROSS_COMPILE="$cross_prefix" \
  CONFIG_EXTRA_LDFLAGS="-Wl,-Ttext-segment=$address" busybox
"$llvm_readelf" -l "$output/busybox" >"$output/program-headers.txt"
grep -E -q "LOAD +0x[0-9a-f]+ +$address" "$output/program-headers.txt"
