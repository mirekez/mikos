#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 SOURCE OUTPUT JOBS OPTIONS" >&2
  exit 2
fi

source_tree="$1"
output="$2"
jobs="$3"
options="$4"
scratch="$(mktemp -d /tmp/mikos-dropbear-host.XXXXXX)"

cleanup() {
  rm -r "$scratch"
}
trap cleanup EXIT

# Use the already downloaded and MikOS-patched source, but configure a clean
# native copy so the RV32 server's in-tree build is not disturbed.
cp -a "$source_tree/." "$scratch/"
make -C "$scratch" distclean >/dev/null 2>&1 || true
rm -rf "$scratch/obj"
install -m 0644 "$options" "$scratch/localoptions.h"

(
  cd "$scratch"
  ./configure --disable-zlib --disable-syslog --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx
  make -j"$jobs" PROGRAMS='dbclient dropbearconvert' \
    dbclient dropbearconvert
  ./dbclient -Q cipher | rg -qx none
)

install -d "$output"
install -m 0755 "$scratch/dbclient" "$output/dbclient"
install -m 0755 "$scratch/dropbearconvert" "$output/dropbearconvert"
touch "$output/.built"
