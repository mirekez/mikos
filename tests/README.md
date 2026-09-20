# Tests

`make test` from the repository root builds and runs the host regression tests
and compile-checks the IA-32 VirtIO transport. Regression sources are grouped
by the subsystem they exercise:

- `kernel/` covers core value types, the Linux RV32 ABI, scheduling, and kernel
  binary inspection;
- `drivers/` covers reusable device and transport logic;
- `fs/loadfs_test.cpp` is the reusable mutable-filesystem contract load,
  exercising PRBS create/read/move/concatenate/delete consistency;
- `net/` covers protocol parsing and response generation;
- [`network/`](network/README.md) exercises the production TCP stack with
  deterministic windows, loss, timers, options, floods, and cleanup, plus an
  optional real Linux peer;
- `qemu/` contains target acceptance runners and their host-side helpers;
- `tribe/` builds cpphdl's Tribe CPU and runs its rootless board acceptance;
- `regression/socket.md` and `regression/ioctl.md` list the implemented and
  target-level regression cases for each networking control syscall;
- `busybox/` owns the external BusyBox/stress-ng compatibility workload and
  the ext4 root image used by every MikOS target runner.

Any `kernel/*_test.cpp`, `drivers/*_test.cpp`, `fs/*_test.cpp`, or
`net/*_test.cpp` or `network/*_test.cpp` file is automatically built as a separate regression binary
by `tests/Makefile`.
Related cases may share one test source. Keep QEMU acceptance tests separate
from host regressions so `make test` remains fast and deterministic.
