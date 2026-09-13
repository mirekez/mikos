#!/usr/bin/env bash
set -euo pipefail
prefix="${1:?usage: check_linux_toolchain.sh CROSS_PREFIX}"
if ! command -v "${prefix}gcc" >/dev/null; then
  echo "RV32 Linux toolchain missing: ${prefix}gcc" >&2
  echo 'Run make userspace-toolchain from the mikOS root, or set RISCV_LINUX_PREFIX to an installed RV32/ILP32 glibc toolchain.' >&2
  echo 'BusyBox and Dropbear run on mikOS but need a Linux-ABI C library; no Linux guest kernel is needed.' >&2
  exit 2
fi
scratch="$(mktemp -d /tmp/mikos-toolchain-check.XXXXXX)"
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/check.c" <<'EOF'
#define _GNU_SOURCE
#include <features.h>
#include <limits.h>
#include <stdio.h>
#if !defined(__GLIBC__) || !defined(__riscv) || __riscv_xlen != 32
#error mikOS userspace requires RV32 glibc
#endif
#if !defined(__riscv_float_abi_soft) || defined(__riscv_flen)
#error Tribe userspace requires ILP32 without floating-point instructions
#endif
#if !defined(LONG_BIT) || LONG_BIT != 32
#error Complete GCC system headers are required; finish the final compiler stage
#endif
int main(void) { return puts("mikOS toolchain check") < 0; }
EOF
if ! "${prefix}gcc" -static "$scratch/check.c" -o "$scratch/check" > "$scratch/log" 2>&1; then
  echo "RV32 Linux toolchain unusable: ${prefix}gcc" >&2
  cat "$scratch/log" >&2
  echo 'A complete RV32/ILP32 glibc toolchain with static libraries is required. Run make userspace-toolchain, or set RISCV_LINUX_PREFIX.' >&2
  exit 2
fi
