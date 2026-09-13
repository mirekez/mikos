#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
source "$root/tests/tribe/paths.sh"
jobs="${JOBS:-2}"
riscv_home="${RISCV_HOME:-$HOME/riscv}"
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

build_tree="$(tribe_build_directory "$target")"
if [[ -z "${CPPHDL_HOME:-}" ]]; then
  echo 'Set CPPHDL_HOME to the cpphdl checkout containing the Tribe bug fixes (for example: export CPPHDL_HOME="$HOME/cpphdl").' >&2
  exit 1
fi
if [[ ! -f "$CPPHDL_HOME/tribe_cpu/CMakeLists.txt" ]]; then
  echo "CPPHDL_HOME must point to the cpphdl source checkout, not its build directory (for example: export CPPHDL_HOME=\"\$HOME/cpphdl\")." >&2
  exit 1
fi
source_tree="$(cd "$CPPHDL_HOME" && pwd)"
if ! grep -Fq 'TRIBE_CFG_MMU_TLB' "$source_tree/tribe_cpu/CMakeLists.txt"; then
  echo "The cpphdl checkout at $source_tree is missing the required TRIBE_CFG_* CMake options; update the checkout." >&2
  exit 1
fi
toolchain="${CPPHDL_TOOLCHAIN:-$source_tree/.conda}"
features=0
[[ "$target" == tribe64_multicore ]] && features=1
feature_arguments=(-DTRIBE_CFG_RV32IA="$features" -DTRIBE_CFG_ISR="$features"
                     -DTRIBE_CFG_MMU_TLB="$features")
echo "Using current cpphdl sources: $source_tree (including working-tree edits)"

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
  -DTRIBE_IO_REGION_SIZE_CONFIG=4194304 \
  "${feature_arguments[@]}"

RISCV_HOME="$riscv_home" \
  "$toolchain/bin/cmake" --build "$build_tree" --target "$target" -j"$jobs"

echo "Prepared minimal Tribe simulator at $build_tree/$target/$target"
