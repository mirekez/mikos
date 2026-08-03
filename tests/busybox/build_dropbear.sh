#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 6 ]]; then
  echo "usage: $0 SOURCE CROSS_PREFIX ADDRESS LLVM_READELF JOBS OPTIONS" >&2
  exit 2
fi

source_tree="$1"
cross_prefix="$2"
address="$3"
llvm_readelf="$4"
jobs="$5"
options="$6"
host="$(basename "$cross_prefix")"
host="${host%-}"

if [[ -f "$source_tree/Makefile" ]]; then
  make -C "$source_tree" distclean >/dev/null 2>&1 || true
fi
rm -rf "$source_tree/obj"
install -m 0644 "$options" "$source_tree/localoptions.h"

(
  cd "$source_tree"
  CC="${cross_prefix}gcc" AR="${cross_prefix}ar" \
    RANLIB="${cross_prefix}ranlib" STRIP="${cross_prefix}strip" \
    LDFLAGS="-Wl,-Ttext-segment=$address" \
    ./configure --host="$host" --enable-static --disable-zlib \
      --disable-syslog --disable-lastlog --disable-utmp \
      --disable-utmpx --disable-wtmp --disable-wtmpx
  make -j"$jobs" PROGRAMS=dropbear STATIC=1 dropbear
)

"${cross_prefix}strip" "$source_tree/dropbear"
"$llvm_readelf" -l "$source_tree/dropbear" \
  >"$source_tree/program-headers.txt"
rg -q "LOAD +0x[0-9a-f]+ +$address" "$source_tree/program-headers.txt"
