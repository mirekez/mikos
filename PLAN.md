# Mikos Development Plan

This plan turns [REQUIREMENTS.md](REQUIREMENTS.md) into small, testable
milestones. Work does not advance past a gate by replacing failed tests with
skips.

## Working method

For each vertical slice:

1. write the host-side behavioral test or target acceptance test;
2. add the minimum architecture-neutral interface needed by that test;
3. implement one reference path;
4. run binary inspection and all earlier tests;
5. port the slice to the other active architecture profile;
6. update the syscall/support manifest and relevant decision record.

The first executable target is `rv32-flat`, selected because the supplied static
BusyBox and toolchain provide an immediate end-to-end compatibility test. It
also has a true bare address mode. Architecture APIs and ABI metadata for RV64,
IA-32, and AMD64 are created before feature work grows, then those targets are
brought up in successive gates.

Current implementation status: the RV32 POC boots without address translation
or enabled device interrupts, configures PMP, uses one M-mode scheduling timer
to preempt an uncooperative U-mode loop, and passes
`busybox echo MIKOS_BUSYBOX_OK`. See `docs/poc-rv32.md` and
`docs/adr/0001-scheduling-interrupt.md`.

The first networking slice also passes deterministic host/guest ARP and ICMP
exchanges using a polling-only modern virtio-net MMIO driver. Modern/legacy
queues and the NIC data path are compile-time C++ types shared by the MMIO and
PCI transports. The temporary boot probe is linked into the kernel only until
user tasks and IPC can host the driver and stack as services. No TCP or socket
work is added in kernel context. See `docs/network-poc.md`.

## Immediate network vertical slice

The shortest tested route to the requested QEMU login is:

1. **Complete:** shared polling NIC interface and RV32 virtio-mmio discovery,
   RX, TX, fixed MAC/IP reporting, and an end-to-end ARP acceptance test.
2. **Complete:** IPv4 validation/checksums and ICMP echo, with allocation and
   interrupts kept out of the data path.
3. Add the minimum UDP/TCP state machines required by one server connection;
   test them as pure host-side state machines before target integration.
4. Add Linux socket descriptor calls and the small ioctl/netlink compatibility
   view needed for BusyBox `ip addr` to display `10.0.2.15`.
5. Run an existing small SSH server in user space. Do not put SSH framing,
   authentication, or cryptography in the kernel or invent a new SSH protocol
   implementation.
6. Add fixed task records and a bounded `/proc` compatibility view sufficient
   for BusyBox `top`; CPU accounting comes from scheduler boundaries.
7. **Started:** IA-32 PCI discovery plus a polling legacy virtio-net transport
   is compile checked. Add the x86 boot target and rerun the same packet and
   remote-login tests there.

Each gate keeps all previous `make test`, `make qemu-test`, and
`make qemu-net-test` checks green. A serial IP marker or an ARP response is not
reported as SSH or Linux socket compatibility.

## Phase 0 — Resolve the contract

**Goal:** remove contradictions before code makes them expensive.

Deliverables:

- `ADR-0001`: choose the x86-64 policy:
  fixed identity page tables, or no x86-64 strict execution target;
- `ADR-0002`: define the protected flat-memory model for RISC-V PMP and IA-32
  segmentation, including what happens on hardware without either;
- `ADR-0003`: define cooperative scheduling and maximum signal-delivery
  guarantees without interrupts;
- `ADR-0004`: define the kernel/user formatting policy and whether direct
  `std::format` can ever meet the no-exception kernel rule;
- confirm the Linux 6.19 reference pin and checked-in input hashes documented in
  `docs/linux-reference.md`, then select the first libc and executable format
  profile;
- define the initial L1 application suite and quantitative success criteria;
- write a short threat model and explicitly distinguish functional
  compatibility from security isolation.

Tests written first:

- compiler and C++26 feature-probe tests;
- a compile/link test proving `-fno-exceptions -fno-rtti`;
- a `std::format` host probe and a kernel-runtime suitability probe;
- cross-target ABI size/alignment probes;
- QEMU availability and timeout harness self-tests.

Exit gate:

- all five hardware/compatibility constraints in the requirements have recorded
  decisions;
- `clang++` from `.conda/bin` is selected and the build rejects fallback to a
  host compiler;
- the build/test skeleton passes from a clean checkout.

## Phase 1 — Freestanding skeleton and boot protocol

**Goal:** boot `rv32-flat` into C++ and report deterministic test results.

Deliverables:

- repository layout for `kernel`, `arch`, `platform`, `services`, `user`,
  `tests`, `tools`, `generated`, and `docs/adr`;
- freestanding runtime: entry, linker script, zero/copy initialization, stack,
  panic, bounded trace records, and no global constructors;
- typed architecture interface for CPU state, privilege transition, cycle
  counter, IRQ masking, and shutdown;
- machine-readable test/boot protocol;
- reproducible image and QEMU runner.

Tests written first:

- linker-layout and boot-header tests;
- zero/BSS initialization and stack-alignment target tests;
- deliberate panic record test;
- forbidden-symbol/unwind/RTTI/constructor inspection tests;
- boot timeout and malformed-output harness tests.

Exit gate:

- `rv32-flat` boots, runs target tests, and shuts down deterministically;
- `satp` is verified Bare and maskable interrupt enables remain clear;
- binary inspection passes.

## Phase 2 — Core value types and flat physical memory

**Goal:** make memory ownership explicit before adding tasks or IPC.

Deliverables:

- strongly typed physical addresses, byte counts, alignment, region kinds, and
  checked arithmetic;
- boot memory-map normalization;
- deterministic region allocator with no overcommit;
- user-range validator and copy-in/copy-out primitives;
- protection-backend interface with initial RISC-V PMP implementation;
- bounded kernel allocation policy.

Tests written first:

- property tests for overflow, alignment, split/merge, and overlap;
- malformed and adversarial memory-map corpus;
- allocation exhaustion and ownership-transition tests;
- user pointer boundary tests;
- target negative-access tests for kernel and neighboring user regions.

Exit gate:

- randomized host tests pass under sanitizers;
- target protection tests demonstrate the exact isolation claimed by
  `rv32-flat`;
- no kernel path added so far has unbounded allocation.

## Phase 3 — Pending-event queue and cooperative execution

**Goal:** establish timer-only user preemption and the polling notification
mechanism.

Deliverables:

- bounded lock-free MPSC per-CPU queue;
- event classes, coalescing, overflow counters, and fatal overflow policy;
- safe-point dispatcher with cycle-budget accounting;
- user context creation, cooperative yield, block, wake, and teardown;
- minimal unexpected-IRQ and fatal-machine-event stubs.
- sole per-CPU scheduling timer with a bounded user-only handler.

Tests written first:

- deterministic queue state-machine and wraparound tests;
- model/property tests for multiple producer interleavings;
- target proof that required atomics are lock-free;
- queue saturation and overflow-policy tests;
- context register/stack preservation tests;
- interrupt-mask persistence and synthetic unexpected-vector tests;
- standard-load test for the 1% event-cycle budget.

Exit gate:

- queue correctness tests pass on host and target;
- no producer blocks and no overflow is silent;
- two user contexts cooperatively alternate without register corruption;
- CPU and controller mask state is continuously checked.

## Phase 4 — IPC and first user-space service

**Goal:** prove that policy and device behavior can remain outside the kernel.

Deliverables:

- bounded capability/handle table;
- fixed-size IPC request/reply and notification operations;
- service lifecycle and failure reporting;
- polled debug-console service as the first driver;
- bootstrap mechanism for the initial user services.

Tests written first:

- stale handle, rights, ownership, and message-bound tests;
- service crash, timeout, and malformed-reply tests;
- end-to-end user-to-service console output test;
- backpressure and fairness tests at cooperative safe points.

Exit gate:

- a user task prints through an unprivileged polled service;
- killing or corrupting the service does not corrupt the kernel;
- kernel policy remains limited to IPC mechanism and resource validation.

## Phase 5 — Generated Linux syscall surface

**Goal:** recognize the complete pinned syscall table on all four ABIs.

Deliverables:

- reproducible Linux UAPI importer/generator;
- a configurable `--linux-source`/`MIKOS_LINUX_SOURCE` regeneration path with
  no dependency on a developer-specific absolute path;
- a checked-in source-input manifest containing revision, relative path,
  SHA-256, SPDX identifier, and ABI use;
- syscall metadata and register-decode definitions for i386, x86-64, riscv32,
  and riscv64;
- architecture-neutral validated dispatch;
- `ENOSYS` stubs and generated support manifest;
- syscall tracing by numeric ID without formatting in the hot path.

Tests written first:

- golden comparison against pinned UAPI inputs;
- duplicate, gap, alias, and generator-reproducibility tests;
- generated switch/table exhaustiveness tests;
- register decode, sign extension, structure layout, and negative-errno fixtures;
- fuzz tests for argument decoding and invalid pointers.

Exit gate:

- every selected Linux syscall number is recognized or intentionally classified
  on all four ABIs;
- unsupported calls return exactly `-ENOSYS`;
- two generator runs are byte-identical;
- L0 compatibility manifests are published.

## Phase 6 — L1 application vertical slice

**Goal:** run static single-threaded Linux ABI programs.

Implement in thin vertical slices, test first:

1. executable validation and bootstrap loading;
2. `exit`/`exit_group`;
3. console `write` through the user service;
4. `brk` and constrained anonymous `mmap`/`munmap`;
5. file handles plus `openat`, `close`, `read`, `write`, and `lseek`;
6. `fstat`/`newfstatat` and directory access;
7. monotonic time and sleep expressed through cooperative polling;
8. basic identity, `uname`, and process metadata calls.

Deliverables:

- bootstrap ELF loader, followed by a user-space loader service;
- RAM filesystem service;
- syscall-to-service forwarding and cancellation;
- statically linked smoke applications and chosen libc bring-up;
- differential tests comparing applicable behavior with Linux.

Exit gate:

- the declared L1 suite runs without source changes on `rv32-flat`;
- syscall results and structure layouts match golden Linux fixtures;
- all limitations of flat `mmap`/`brk` semantics appear in the manifest.

## Phase 7 — Second and third architecture bring-up

**Goal:** prove that the core was portable rather than merely abstract-looking.

Order:

1. `rv64-flat`;
2. `x86-32-flat`;
3. `x86-64-fixed`, only if enabled by `ADR-0001`.

Each port requires:

- boot and linker implementation;
- privilege transition and protection backend;
- syscall entry/return and fault frames;
- atomic and cycle-counter validation;
- IRQ/controller disable verification;
- full Phases 1–6 target tests.

Additional IA-32 tests MUST cover segmentation limits, call/syscall entry,
descriptor-table integrity, and paging-disabled state. Any fixed AMD64 profile
MUST test that mappings are immutable and identity-only and MUST be labeled
non-strict in compatibility reports.

Exit gate:

- the L1 suite passes on RV32, RV64, and IA-32;
- AMD64 has either a passing, accurately labeled fixed-map profile or a
  documented hardware exclusion;
- shared code contains no architecture-dependent address-width assumptions.

## Phase 8 — L2 processes, threads, signals, and dynamic programs

**Goal:** support richer applications within the cooperative flat-memory model.

Candidate order:

- eager-copy `fork` and `vfork` with explicit restrictions;
- `execve`, wait, process groups, and credentials in a process service;
- `clone`/threads, futex subset, and thread-local ABI;
- dynamic ELF interpreter and shared objects;
- polling descriptors and readiness service;
- POSIX signal masks, pending sets, Linux signal frames, and safe-point delivery;
- sockets and a user-space network stack with polled NIC backend.

Tests written first for each slice:

- Linux differential behavior fixtures;
- exhaustion and partial-failure cleanup;
- ABI-correct signal/context restoration;
- data-race and queue-pressure tests;
- adversarial executable and protocol inputs;
- explicit test showing the documented non-delivery interval for a task that
  reaches no safe point.

Exit gate:

- the selected L2 suite passes;
- no result claims transparent preemption or arbitrary-point signal delivery;
- dynamic and threaded application limitations are machine-readable.

## Phase 9 — Compatibility expansion

**Goal:** grow behavior from measured demand, not syscall-count optics.

For each syscall family:

1. select application/LTP failures that require it;
2. capture Linux behavior fixtures;
3. implement policy in a user service where possible;
4. add fault, cancellation, and resource-exhaustion tests;
5. update status from `stubbed` to `partial`, `implemented`, then `conformant`.

Priority families:

- complete VFS metadata and directory behavior;
- pipes, descriptor passing, polling, and terminals;
- time, timers expressed through polling, and accounting;
- sockets and common networking ioctls;
- process/session/job-control behavior;
- synchronization primitives;
- compatibility views for selected `/proc` and `/sys` files.

Exit gate:

- a versioned L3 real-world application suite is green;
- applicable LTP/libc results are published, including failures and exclusions;
- kernel growth for compatibility features requires an ADR showing why a
  user-space implementation is insufficient.

## Phase 10 — Hardware qualification and releases

**Goal:** validate assumptions that emulators cannot prove.

Deliverables:

- board-specific boot, memory map, polled UART/storage/network support;
- PMP/segmentation negative tests on real hardware;
- DMA ownership and cache-coherency tests;
- unavoidable-event and unexpected-IRQ behavior tests;
- reproducible release images, SBOM/toolchain record, support manifests, and
  performance baselines.

Exit gate:

- every released hardware profile passes its target and conformance suite;
- IRQ-off, memory-protection, and event-budget claims are measured on hardware;
- recovery and crash records remain bounded under device and service failures.

## Initial backlog

The first implementation iteration, after Phase 0 decisions, should contain
only these tickets:

1. create the Clang-only CMake presets/toolchain and feature probes;
2. create the host test runner with a deliberately failing sample;
3. add forbidden-symbol and unwind-section inspection;
4. add the RV32 linker-layout test;
5. add a QEMU boot-timeout harness test;
6. boot RV32 assembly into a minimal C++ function;
7. emit one machine-readable passing test record;
8. verify `satp` Bare and interrupt enables clear;
9. replace the sample failure with the first real boot acceptance test.

## Global definition of done

A change is done when:

- its behavior was introduced by a failing test;
- host, target, integration, and inspection tests affected by it pass;
- applicable failure and exhaustion paths are tested;
- public ABI metadata and support status are updated;
- no new exception, unwind, RTTI, constructor, IRQ, page-table, or unbounded
  allocation dependency appears;
- architecture-specific behavior is documented and does not leak into the
  portable core;
- the result is reproducible with the repository's Conda Clang environment.
