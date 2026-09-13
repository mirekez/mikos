#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
riscv_home="${RISCV_HOME:-$HOME/riscv}"
source_tree="${RISCV_TOOLCHAIN_SOURCE:-$riscv_home/riscv-gnu-toolchain}"
prefix="${RISCV_USERSPACE_HOME:-$riscv_home/linux}"
build="${RISCV_TOOLCHAIN_BUILD:-$root/build/toolchains/rv32-linux}"
jobs="${JOBS:-2}"
minimum_free_mib="${RISCV_TOOLCHAIN_MIN_FREE_MIB:-8192}"
if [[ ! "$minimum_free_mib" =~ ^[1-9][0-9]*$ ]]; then
  echo 'RISCV_TOOLCHAIN_MIN_FREE_MIB must be a positive integer.' >&2
  exit 2
fi

for input in configure gcc/configure binutils/configure glibc/configure linux-headers/include/linux/version.h; do
  if [[ ! -f "$source_tree/$input" ]]; then
    echo "Missing toolchain source: $source_tree/$input" >&2
    echo 'Set RISCV_TOOLCHAIN_SOURCE to a populated riscv-gnu-toolchain checkout (see README.md).' >&2
    exit 2
  fi
done
mkdir -p "$build" "$prefix"
source_tree="$(cd "$source_tree" && pwd)"
prefix="$(cd "$prefix" && pwd)"
build="$(cd "$build" && pwd)"
if [[ ! -f "$build/stamps/build-gcc-linux-stage2" ]] &&
    (( $(df -Pk "$build" | awk 'END {print $4}') < minimum_free_mib * 1024 )); then
  echo "The userspace toolchain build requires $minimum_free_mib MiB free at $build (RISCV_TOOLCHAIN_MIN_FREE_MIB)." >&2
  echo 'Free space or set RISCV_TOOLCHAIN_BUILD to a directory on a larger filesystem.' >&2
  exit 2
fi

# Use the host tools, independently of an activated Conda cross-build environment.
# Do not reconfigure or overwrite the user's existing bare-metal toolchain.
export PATH="$prefix/bin:/usr/bin:/bin:$PATH"
unset CC CXX CPP AR AS LD NM OBJCOPY OBJDUMP RANLIB STRIP CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
unset BUILD HOST TARGET CC_FOR_BUILD CXX_FOR_BUILD CPP_FOR_BUILD CONFIG_SITE
unset build_alias host_alias target_alias
export CC=gcc CXX=g++ CFLAGS='-O2 -g0' CXXFLAGS='-O2 -g0'
settings="$source_tree|$prefix|rv32ima|ilp32"
if [[ -f "$build/.mikos-settings" ]] && [[ "$(cat "$build/.mikos-settings")" != "$settings" ]]; then
  echo "Toolchain build configuration changed; choose a new RISCV_TOOLCHAIN_BUILD directory." >&2
  exit 2
fi
(cd "$build" && "$source_tree/configure" --prefix="$prefix" \
  --with-arch=rv32ima --with-abi=ilp32 --with-languages=c,c++ \
  --enable-linux --disable-multilib --disable-gdb --disable-default-pie)
printf '%s\n' "$settings" > "$build/.mikos-settings"

# Complete and install libc first, then reclaim intermediate objects before
# building the final compiler. Keep upstream stamps so retries are incremental.
make -C "$build" -j"$jobs" build-libc
for directory in build-binutils-linux build-gcc-linux-stage1; do
  if [[ -f "$build/$directory/Makefile" ]]; then
    make -C "$build/$directory" clean
  fi
done
# Upstream's stage rule discards a partially built compiler on a retry. Resume
# its configured makefiles instead; these are the same build/install steps as
# the upstream build-gcc-linux-stage2 rule.
if [[ -f "$build/build-gcc-linux-stage2/Makefile" &&
      ! -f "$build/stamps/build-gcc-linux-stage2" ]]; then
  make -C "$build/build-gcc-linux-stage2" -j"$jobs"
  make -C "$build/build-gcc-linux-stage2" install
  cp -a "$prefix/riscv32-unknown-linux-gnu"/lib* "$prefix/sysroot/"
  touch "$build/stamps/build-gcc-linux-stage2"
fi
make -C "$build" -j"$jobs" linux
bash "$root/tests/busybox/check_linux_toolchain.sh" "$prefix/bin/riscv32-unknown-linux-gnu-"
echo "Prepared mikOS userspace toolchain: $prefix/bin/riscv32-unknown-linux-gnu-gcc"
