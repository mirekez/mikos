# Tests

`make test` from the repository root builds and runs the host regression tests
and compile-checks the IA-32 VirtIO transport. Regression sources are grouped
by the subsystem they exercise:

- `kernel/` covers core value types, the Linux RV32 ABI, scheduling, and kernel
  binary inspection;
- `drivers/` covers reusable device and transport logic;
- `net/` covers protocol parsing and response generation;
- `qemu/` contains target acceptance runners and their host-side helpers;
- `busybox/` owns the external BusyBox/stress-ng compatibility workload.

Any `kernel/*_test.cpp`, `drivers/*_test.cpp`, or `net/*_test.cpp` file is
automatically built as a separate regression binary by `tests/Makefile`.
Related cases may share one test source. Keep QEMU acceptance tests separate
from host regressions so `make test` remains fast and deterministic.
