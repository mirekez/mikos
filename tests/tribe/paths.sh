#!/usr/bin/env bash
# Sourced by Tribe entry points after setting root and simulator_name.
tribe_build_directory() {
  printf '%s/%s\n' "${CPPHDL_BUILD_ROOT:-$root/build/tests/tribe/cpphdl-local-build}" "$1"
}
