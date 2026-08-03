#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 DESTINATION VERSION SHA256" >&2
  exit 2
fi

destination="$1"
version="$2"
expected_sha256="$3"
reference="${DROPBEAR_REFERENCE:-}"
repository="${DROPBEAR_REPOSITORY:-https://matt.ucc.asn.au/dropbear/releases}"

if [[ -e "$destination" ]]; then
  echo "Dropbear destination already exists: $destination" >&2
  exit 1
fi

mkdir -p "$(dirname "$destination")"
archive="$(mktemp /tmp/mikos-dropbear.XXXXXX.tar.bz2)"
cleanup() {
  rm -f "$archive"
}
trap cleanup EXIT

if [[ -n "$reference" && -d "$reference" ]]; then
  mkdir -p "$destination"
  tar -C "$reference" --exclude=.git --exclude=obj \
    --exclude='*.o' --exclude=dropbear --exclude=dropbearmulti \
    -cf - . | tar -C "$destination" -xf -
else
  if [[ -n "$reference" ]]; then
    cp "$reference" "$archive"
  else
    curl --fail --location --silent --show-error \
      "$repository/dropbear-$version.tar.bz2" --output "$archive"
  fi
  actual_sha256="$(sha256sum "$archive" | cut -d' ' -f1)"
  if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "Dropbear archive checksum mismatch" >&2
    echo "expected: $expected_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
  fi
  mkdir -p "$destination"
  tar -xjf "$archive" --strip-components=1 -C "$destination"
fi

touch "$destination/.source-ready"
