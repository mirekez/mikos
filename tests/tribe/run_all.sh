#!/usr/bin/env bash
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
results="$root/build/tests/tribe/results"
mkdir -p "$results"
summary="$results/summary.txt"
: >"$summary"
failed=0
run_case() {
  local name="$1" status
  shift
  echo "Running $name (log: $results/$name.log)"
  "$@" >"$results/$name.log" 2>&1
  status=$?
  if ((status == 0)); then
    echo "PASS: $name" | tee -a "$summary"
  else
    failed=1
    if rg -q 'RV32 Linux toolchain missing:|missing Tribe TAP bridge socket:|missing host TAP interface:' "$results/$name.log"; then
      echo "BLOCKED: $name (missing test inputs; exit $status)" | tee -a "$summary"
    else
      echo "FAIL: $name (exit $status)" | tee -a "$summary"
    fi
    tail -n 10 "$results/$name.log"
  fi
  return "$status"
}
run_case host make -C "$root" test
run_case boot-single make -C "$root" tribe-boot-test
run_case kernel-single make -C "$root" tribe-kernel-test
if run_case prepare-multicore bash "$root/tests/tribe/prepare_cpphdl.sh" --multicore; then
  run_case boot-multicore bash "$root/tests/tribe/run_boot.sh" --multicore
  run_case kernel-multicore bash "$root/tests/tribe/run_kernel.sh" --multicore
else
  echo 'BLOCKED: boot-multicore (simulator preparation failed)' | tee -a "$summary"
  echo 'BLOCKED: kernel-multicore (simulator preparation failed)' | tee -a "$summary"
fi
run_case acceptance make -C "$root" tribe-test
for test in ping tcp process ssh; do
  run_case "interactive-$test" make -C "$root" "tribe-interactive-$test-test"
done
echo "Suite results: $summary"
exit "$failed"
