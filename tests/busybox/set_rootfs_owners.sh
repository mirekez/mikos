#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 STAGING_ROOT IMAGE DEBUGFS" >&2
  exit 2
fi

staging_root="${1%/}"
image="$2"
debugfs="$3"

while IFS= read -r -d '' entry; do
  path="${entry#"$staging_root"}"
  if [[ -z "$path" ]]; then
    path=/
  fi
  "$debugfs" -w -R "set_inode_field $path uid 0" "$image" \
    >/dev/null 2>&1
  "$debugfs" -w -R "set_inode_field $path gid 0" "$image" \
    >/dev/null 2>&1
done < <(find "$staging_root" -print0)
