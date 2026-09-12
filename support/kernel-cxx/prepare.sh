#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
dest="$root/build/kernel-cxx"
mkdir -p "$dest"
# Serialize preparation across make invocations, including host regressions.
exec 9>"$dest/.lock"
flock 9
fetch() {
  local name="$1" sha="$2" url="$3"
  if [[ ! -f "$dest/$name" ]]; then
    curl --fail --location --retry 3 --output "$dest/$name.part" "$url"
    mv "$dest/$name.part" "$dest/$name"
  fi
  printf '%s  %s\n' "$sha" "$dest/$name" | sha256sum --check --status
}
fetch libcxx.tar.xz 598f301eda6ae83e0a9cb219933d01ac0a298c78d04f43a7d017a5b8b3346c71 \
  https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.3/libcxx-21.1.3.src.tar.xz
fetch inplace-vector.tar.gz 7ab710f1faa6e929672d303b87d5b78b46bc263856c43006ec1341053dc6554b \
  https://codeload.github.com/bemanproject/inplace_vector/tar.gz/63569fe8502c3504d7d81bb6d378f3ec21fb1e95
fetch compiler-rt.tar.xz 143b6ac5278788a9010f2464e77627945c1633706bb571b664e14a5350670d93 \
  https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.3/compiler-rt-21.1.3.src.tar.xz
if [[ ! -f "$dest/libcxx-21.1.3.src/include/vector" ]]; then
  tar -xJf "$dest/libcxx.tar.xz" -C "$dest"
fi
if [[ ! -f "$dest/inplace_vector-63569fe8502c3504d7d81bb6d378f3ec21fb1e95/include/beman/inplace_vector/inplace_vector.hpp" ]]; then
  tar -xzf "$dest/inplace-vector.tar.gz" -C "$dest"
fi
if [[ ! -f "$dest/compiler-rt-21.1.3.src/lib/builtins/divsf3.c" ]]; then
  tar -xJf "$dest/compiler-rt.tar.xz" -C "$dest"
fi
cmake -DSOURCE="$dest/libcxx-21.1.3.src" -DOUTPUT="$dest/include" \
  -P "$root/support/kernel-cxx/configure.cmake"
touch "$dest/.ready"
