#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 DESTINATION COMMIT" >&2
  exit 2
fi

destination="$1"
commit="$2"
reference="${BUSYBOX_REFERENCE:-}"
repository="${BUSYBOX_REPOSITORY:-https://git.busybox.net/busybox}"

if [[ -e "$destination" ]]; then
  echo "BusyBox destination already exists: $destination" >&2
  exit 1
fi

mkdir -p "$(dirname "$destination")"
if [[ -n "$reference" ]]; then
  mkdir -p "$destination"
  git -C "$reference" archive --format=tar "$commit" |
    tar -xf - -C "$destination"
else
  git clone --filter=blob:none --no-checkout "$repository" "$destination"
  git -C "$destination" checkout --detach "$commit"
fi

touch "$destination/.source-ready"
