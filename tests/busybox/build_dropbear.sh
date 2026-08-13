#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 7 ]]; then
  echo "usage: $0 SOURCE CROSS_PREFIX ADDRESS LLVM_READELF JOBS OPTIONS PATCH" >&2
  exit 2
fi

source_tree="$1"
cross_prefix="$2"
address="$3"
llvm_readelf="$4"
jobs="$5"
options="$6"
patch_file="$7"
host="$(basename "$cross_prefix")"
host="${host%-}"

if [[ -f "$source_tree/Makefile" ]]; then
  make -C "$source_tree" distclean >/dev/null 2>&1 || true
fi
rm -rf "$source_tree/obj"
install -m 0644 "$options" "$source_tree/localoptions.h"
if patch -d "$source_tree" -p1 --forward --dry-run \
    <"$patch_file" >/dev/null 2>&1; then
  patch -d "$source_tree" -p1 <"$patch_file"
elif ! patch -d "$source_tree" -p1 --reverse --dry-run \
    <"$patch_file" >/dev/null 2>&1; then
  echo "Dropbear MikOS patch does not apply cleanly" >&2
  exit 1
fi

# SSH key exchange is CPU-bound under Tribe's cycle-level native C++ model.
# Optimizing this guest code for size makes Curve25519 take many times longer
# in wall time, while section garbage collection still keeps unused algorithms
# out of the embedded image.
(
  cd "$source_tree"
  CC="${cross_prefix}gcc" AR="${cross_prefix}ar" \
    RANLIB="${cross_prefix}ranlib" STRIP="${cross_prefix}strip" \
    CFLAGS="-O3 -fomit-frame-pointer -ffunction-sections -fdata-sections" \
    LDFLAGS="-Wl,--gc-sections -Wl,-Ttext-segment=$address" \
    ./configure --host="$host" --enable-static --disable-zlib \
      --disable-syslog --disable-lastlog --disable-utmp \
      --disable-utmpx --disable-wtmp --disable-wtmpx
  make -j"$jobs" PROGRAMS=dropbear STATIC=1 dropbear
)

"${cross_prefix}strip" "$source_tree/dropbear"
"$llvm_readelf" -l "$source_tree/dropbear" \
  >"$source_tree/program-headers.txt"
rg -q "LOAD +0x[0-9a-f]+ +$address" "$source_tree/program-headers.txt"
