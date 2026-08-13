#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$root/build/tests/tribe"
source_tree="$work/cpphdl-source"
build_tree="$work/cpphdl-build"
repository="${CPPHDL_REPOSITORY:-https://github.com/mirekez/cpphdl.git}"
reference="${CPPHDL_REFERENCE:-}"
revision="${CPPHDL_REVISION:-d131e7b670e5b69b8df322ca0adfe9f714446494}"
jobs="${JOBS:-2}"
toolchain="${CPPHDL_TOOLCHAIN:-$root/.conda}"
riscv_home="${RISCV_HOME:-/home/me/riscv}"
target="tribe64"

usage() {
  echo "usage: $0 [--multicore]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      target="tribe64_multicore"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

mkdir -p "$work"
if [[ ! -d "$source_tree/.git" ]]; then
  git clone --no-checkout "${reference:-$repository}" "$source_tree"
fi

if ! git -C "$source_tree" cat-file -e "$revision^{commit}" 2>/dev/null; then
  git -C "$source_tree" fetch --depth 1 origin "$revision"
fi
git -C "$source_tree" checkout --detach --force "$revision"

config="$source_tree/tribe/Config.h"
if [[ "$target" == "tribe64" ]]; then
  for feature in ENABLE_RV32IA ENABLE_ISR ENABLE_MMU_TLB; do
    sed -i "s/^#define ${feature}\(.*\)$/\/\/ ${feature} disabled for MikOS\1/" \
      "$config"
  done

  for feature in ENABLE_RV32IA ENABLE_ISR ENABLE_MMU_TLB; do
    if rg -q "^#define ${feature}\\b" "$config"; then
      echo "FAIL: $feature remains enabled in Tribe Config.h" >&2
      exit 1
    fi
  done
else
  for feature in ENABLE_RV32IA ENABLE_ISR ENABLE_MMU_TLB; do
    if ! rg -q "^#define ${feature}\\b" "$config"; then
      echo "FAIL: $feature is required by multicore Tribe" >&2
      exit 1
    fi
  done
fi
for feature in ENABLE_ZICSR ENABLE_TRAPS; do
  if ! rg -q "^#define ${feature}\\b" "$config"; then
    echo "FAIL: $feature is required by MikOS user-mode ecalls" >&2
    exit 1
  fi
done

# Upstream currently wires CSR inputs unconditionally when ISR ports are
# absent, and enables stacktrace from header presence without honoring its
# failed CMake link probe. Keep both compatibility fixes local to this clone.
git -C "$source_tree" apply "$root/tests/tribe/patches/no-isr-csr-time.patch"
git -C "$source_tree" apply \
  "$root/tests/tribe/patches/polling-dma-invalidate.patch"
git -C "$source_tree" apply \
  "$root/tests/tribe/patches/preserve-host-control-frames.patch"
git -C "$source_tree" apply \
  "$root/tests/tribe/patches/full-ethernet-frame-rx.patch"
git -C "$source_tree" apply \
  "$root/tests/tribe/patches/verilator-memory-config.patch"
git -C "$source_tree" apply \
  "$root/tests/tribe/patches/multicore-clint-hart0.patch"
git -C "$source_tree" apply \
  "$root/tests/tribe/patches/native-port-cache-fast-path.patch"

"$toolchain/bin/cmake" -S "$source_tree" -B "$build_tree" \
  -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$toolchain/bin/clang" \
  -DCMAKE_CXX_COMPILER="$toolchain/bin/clang++" \
  -DCMAKE_MAKE_PROGRAM="$toolchain/bin/make" \
  -DCPPHDL_LOCAL_CONDA_PREFIX="$toolchain" \
  -DCPPHDL_USE_LOCAL_CONDA=ON \
  -DCPPHDL_BUILD_EXAMPLES=OFF \
  -DCPPHDL_BUILD_TESTS=OFF \
  -DCPPHDL_BUILD_TRIBE=ON \
  -DBUILD_TESTING=OFF \
  -DTRIBE_RAM_BYTES_CONFIG=33554432 \
  -DTRIBE_IO_REGION_SIZE_CONFIG=4194304

RISCV_HOME="$riscv_home" \
  "$toolchain/bin/cmake" --build "$build_tree" --target "$target" -j"$jobs"

echo "Prepared minimal Tribe simulator at $build_tree/$target/$target"
