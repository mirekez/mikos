#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
exec bash "$root/tests/tribe/run_tribe.sh" --kernel-only "$@"
