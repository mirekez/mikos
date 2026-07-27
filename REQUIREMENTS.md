# Mikos Operating System Requirements

Status: initial architecture baseline  
Language: C++26 and minimal architecture-specific assembly  
Primary toolchain: Clang/LLVM from the repository's Conda environment

Primary Linux reference: Linux 6.19, commit
`05f7e89ab9731565d8a62e3b5d1ec206485eeb0b`, available during initial
development at `/home/me/cpphdl/tribe/linux/linux-build/linux`. Exact reference
inputs are recorded in [docs/linux-reference.md](docs/linux-reference.md).

## 1. Purpose and scope

Mikos is a small, test-driven, Unix-like operating system that exposes Linux
system-call ABIs while keeping policy, drivers, filesystems, networking, and
other services outside the kernel.

The project has two distinct compatibility objectives:

1. **ABI coverage:** every syscall number in each selected Linux architecture
   table is generated, recognized, traced, and dispatched. An unimplemented
   syscall returns `-ENOSYS`.
2. **Behavioral compatibility:** implemented calls match observable Linux
   behavior closely enough to run defined application and conformance suites.

A complete syscall table is achievable. Running *every* Linux application
unmodified is not a technically testable or achievable requirement under a
strict no-MMU, no-interrupt design. Some programs require demand paging,
copy-on-write `fork`, fixed virtual address spaces, asynchronous preemption,
kernel Linux drivers, containers, or privileged Linux interfaces. Compatibility
must therefore be reported by syscall, application suite, and architecture
profile rather than as a single yes/no claim.

## 2. Normative language

`MUST`, `MUST NOT`, `SHOULD`, and `MAY` are normative. A requirement is complete
only when its stated verification is automated or an explicitly named hardware
test is documented.

## 3. Design principles

- **P-001 Minimal kernel:** only mechanisms that require privileged execution
  belong in the kernel. Policy belongs in replaceable user-space services.
- **P-002 Polling first:** devices and ordinary events use polling and explicit
  safe points. The only normal maskable interrupt is the per-CPU scheduling
  timer defined by `IRQ-001`.
- **P-003 Flat memory:** the strict profile uses physical/linear addresses
  without page translation, page tables, demand paging, or page faults for
  memory management.
- **P-004 Explicit failure:** recoverable failures use values such as
  `std::expected`, status types, or negative Linux `errno`; C++ exceptions are
  disabled.
- **P-005 Two runtime privilege domains:** the system exposes only kernel and
  user domains. It does not expose a hypervisor or guest domain.
- **P-006 Portable core:** architecture-independent code is shared; privileged
  entry, context, boot, memory protection, and device access are behind narrow
  architecture interfaces.
- **P-007 Tests define progress:** no feature is considered supported before
  its tests pass on every architecture profile to which it applies.
- **P-008 Any exception (or one interrupt) handling must have maximum depth=1
  without any preemption possible. Purpose of exception is terminal state only

## 4. Feasibility constraints and required decisions

These are hardware constraints, not implementation preferences:

- **C-001 x86-64 long mode requires paging.** A strict no-page-table x86-64
  kernel cannot execute the x86-64 ISA. Before x86-64 bring-up, the project MUST
  select one of:
  - an identity-mapped, fixed page-table compatibility profile with no demand
    paging or runtime remapping; or
  - omission of x86-64 execution from the strict profile.
- **C-002 Flat addressing does not itself isolate programs.** RISC-V PMP or
  another physical-memory protection mechanism, and x86 segmentation where
  applicable, are required for meaningful kernel/user isolation. Where hardware
  cannot enforce the declared regions, the profile MUST be labeled
  `unprotected`; it MUST NOT claim security isolation.
- **C-003 not every CPU event is maskable.** NMI, SMI, reset, machine-check, and
  synchronous faults cannot all be disabled. They need minimal fail-stop or
  crash-handling vectors even though ordinary device IRQs remain masked.
- **C-004 transparent preemption requires one asynchronous trap.** A pending
  flag cannot regain control from an unmodified user task. Strict profiles
  therefore permit one architecture-local scheduling timer interrupt. It may
  preempt U-mode only; the kernel runs with its global interrupt-enable bit
  clear. See [ADR-0001](docs/adr/0001-scheduling-interrupt.md).
- **C-005 hosted C++ is not a freestanding kernel runtime.** `std::format` may
  allocate and its specified error path uses `std::format_error`. Kernel use is
  permitted only through a checked, no-failure wrapper with a proven compatible
  standard library/runtime; otherwise `std::format` is limited to host tools and
  user-space services, while the kernel uses a bounded non-throwing formatter.

The decisions above are Phase 0 exit criteria in [PLAN.md](PLAN.md).

## 5. Supported profiles and architectures

### 5.1 Architecture profiles

| Profile | ISA | Address width | Translation | Initial status |
|---|---|---:|---|---|
| `rv32-flat` | RISC-V RV32 | 32 | `satp` Bare | first POC target |
| `rv64-flat` | RISC-V RV64 | 64 | `satp` Bare | required port |
| `x86-32-flat` | IA-32 | 32 | paging disabled | required port |
| `x86-64-fixed` | AMD64 | 64 | fixed identity map | decision-gated |

All four ABIs MUST be represented in the source tree from the first syscall
table generation milestone. Executable bring-up is staged as described in the
plan.

### 5.2 Endianness and platform assumptions

- Initial targets are little-endian.
- Initial hardware support MAY target QEMU `virt`/`pc` machines, but production
  code MUST NOT require a hypervisor.
- The first release MAY be uniprocessor. Per-CPU interfaces and event queues
  MUST avoid global assumptions that prevent later SMP support.
- Required integer widths, alignment, ABI layout, and atomic lock-free
  properties MUST be checked at compile time and in boot tests.

## 6. Kernel boundary

### 6.1 Kernel responsibilities

The kernel MUST contain only:

- early boot, CPU initialization, and transition to the user domain;
- privilege entry/exit and synchronous fault handling;
- physical memory discovery and region allocation;
- creation and validation of user memory regions;
- cooperative task contexts and scheduling mechanism;
- bounded IPC primitives;
- the per-CPU pending-event mechanism;
- Linux syscall entry, argument capture, dispatch, and forwarding;
- minimal clock/cycle-counter access needed for accounting;
- crash capture and deterministic shutdown/reboot paths;
- the smallest architecture and platform code needed to provide the above.

### 6.2 User-space responsibilities

The following MUST be user-space services unless hardware makes a tiny
privileged stub unavoidable:

- device drivers and polling loops;
- filesystem and VFS policy;
- executable loading after the initial bootstrap loader;
- networking stacks and protocols;
- process policy, names, credentials, sessions, and job control;
- Linux compatibility behavior that can be implemented through IPC;
- logging sinks, shells, utilities, and system startup policy.

Privileged stubs MUST be capability-scoped, bounded, non-blocking, and tested.

## 7. Interrupt-free operation and pending events

- **IRQ-001:** all ordinary device interrupts and interrupt-controller sources
  MUST remain disabled after boot. The sole exception is one per-CPU scheduling
  timer: RISC-V M-mode timer or an x86 local-APIC timer with every other vector
  masked.
- **IRQ-002:** devices MUST default to polling mode. Bus mastering and DMA MAY be
  used only with explicit ownership, bounds validation, and completion polling.
- **IRQ-003:** every installed vector/trap entry MUST be classified as
  scheduling timer, synchronous fault, unavoidable machine event, syscall
  entry, or unexpected IRQ.
- **IRQ-003A:** the scheduling-timer handler MUST only acknowledge/rearm the
  timer, update bounded accounting, and optionally replace the active user trap
  frame with another ready task's frame. It MUST NOT perform device work,
  allocate, format, block, or run general event handlers.
- **IRQ-003B:** a scheduling timer arriving while the kernel executes MUST NOT
  interrupt kernel work. A pending timer may trap immediately after the next
  return to U-mode.
- **IRQ-004:** an unexpected maskable IRQ stub MUST acknowledge/mask the source
  when safely possible, record a bounded diagnostic, preserve the interrupted
  context, and return without scheduling policy. Repeated storms MUST fail
  closed rather than livelock.
- **IRQ-005:** unavoidable fatal events MAY enter a crash-only path. Recovery
  with C++ exceptions is forbidden.
- **EVT-001:** each CPU MUST own a bounded, lock-free multi-producer,
  single-consumer pending-event queue. Only that CPU consumes its queue.
- **EVT-002:** producers MUST NOT block. Overflow behavior MUST be explicit per
  event class: coalesce, set an overflow bit, drop with a counter, or escalate a
  fatal condition. Silent loss is forbidden.
- **EVT-003:** event publication MUST define and test its C++ atomic memory
  ordering. Required atomics MUST be lock-free on the target; hidden runtime
  locks are forbidden in the kernel.
- **EVT-004:** the consumer runs at syscall boundaries, scheduler yield points,
  IPC boundaries, user-return boundaries, and explicit polling checkpoints.
- **EVT-005:** event handling SHOULD consume no more than 1% of measured CPU
  cycles over a configurable accounting window during the standard load test.
  Safety-critical backlog MAY exceed the budget and MUST increment a visible
  over-budget counter.
- **EVT-006:** POSIX/Linux signals are compatibility-service messages delivered
  at a safe user-return point. Signal frame layout MUST match the selected Linux
  ABI.

## 8. Memory model

- **MEM-001:** strict profiles MUST run with address translation disabled.
- **MEM-002:** the kernel MUST NOT create or mutate PTEs in strict profiles.
- **MEM-003:** physical memory is managed as typed, aligned, non-overlapping
  regions. Ownership transitions MUST be validated.
- **MEM-004:** kernel, user task, shared IPC, DMA, firmware, and device regions
  MUST be represented explicitly.
- **MEM-005:** a user pointer is never dereferenced before overflow-safe range,
  alignment, ownership, and access validation.
- **MEM-006:** no overcommit, swapping, demand paging, or copy-on-write is
  permitted in strict profiles.
- **MEM-007:** `mmap`, `munmap`, `mprotect`, `brk`, `fork`, `clone`, and `execve`
  compatibility MUST document their flat-memory semantics and limitations.
- **MEM-008:** `fork` MAY use an eager physical copy when sufficient memory
  exists. Otherwise it returns the Linux-compatible failure code.
- **MEM-009:** the kernel MUST use bounded allocation after boot. Critical paths
  MUST have preallocated storage and deterministic exhaustion behavior.
- **MEM-010:** executable images MUST be validated for integer overflow,
  overlapping segments, unsupported relocations, and out-of-region addresses.
- **MEM-011:** kernel/user protection MUST use non-page hardware mechanisms
  where available. Protection setup and negative-access tests are required per
  board/CPU profile.

## 9. Privilege, faults, and virtualization

- Exactly two project-visible runtime domains exist: kernel and user.
- The kernel MUST enter the least privileged hardware mode that still provides
  the required protection. Firmware modes used only during boot do not become
  OS domains.
- The OS MUST NOT implement or require a hypervisor, guest mode, nested
  virtualization, or virtual CPUs.
- Emulators MAY be used as development and CI test equipment.
- Synchronous user faults MUST terminate or notify the responsible task without
  corrupting kernel state.
- Kernel faults MUST enter a bounded crash path that records architecture,
  registers, reason, and the last trace records where possible.
- Ordinary control flow MUST NOT use CPU faults, traps, or C++ exceptions as an
  error-handling technique, except the defined syscall/privilege entry trap and
  crash handling.

## 10. Linux compatibility

### 10.1 ABI tables and dispatch

- Syscall definitions MUST be generated from a pinned, recorded Linux UAPI
  version for i386, x86-64, riscv32, and riscv64. The initial version is Linux
  6.19 at the commit recorded above.
- The importer MUST accept the Linux source root as an explicit command-line
  argument or `MIKOS_LINUX_SOURCE` value. It MUST NOT embed a developer home
  directory in generated output or production builds.
- Authoritative imported inputs and their SHA-256 hashes MUST be snapshotted in
  the Mikos repository so a normal build does not depend on the external Linux
  checkout. Regeneration MUST fail on an unexpected hash unless the pin and
  manifest are deliberately updated together.
- Generated entries MUST include syscall number, canonical name, argument
  metadata, architecture ABI, implementation state, and test identifiers.
- Duplicate numbers, gaps, aliases, removed calls, and architecture-specific
  calls MUST be represented intentionally.
- Unknown or unsupported calls return `-ENOSYS`.
- Return values use Linux negative `errno` conventions at the ABI boundary.
- Register preservation, stack alignment, structure layout, time width, signal
  frames, and 32/64-bit conversions MUST have compile-time and executable ABI
  tests.
- Dispatch MUST validate user pointers before forwarding requests.
- A generated machine-readable support manifest MUST distinguish:
  `recognized`, `stubbed`, `partial`, `implemented`, and `conformant`.

### 10.2 Compatibility levels

| Level | Meaning |
|---|---|
| L0 | Complete generated syscall table; unsupported calls return `ENOSYS` |
| L1 | Static, single-threaded ELF programs using core file/memory/time calls |
| L2 | Dynamic loader, threads, polling I/O, signals at safe points, networking |
| L3 | Defined real-world CLI/application suites pass without source changes |
| L4 | Broad Linux Test Project and libc conformance target for applicable calls |

Compatibility MUST always be published as `(architecture profile, level,
test-suite version, pass rate)`.

### 10.3 Initial exclusions

The strict profile does not initially promise:

- applications requiring signal latency tighter than the configured scheduling
  quantum;
- Linux kernel modules, Linux drivers, eBPF in kernel mode, `io_uring`, `perf`,
  `ptrace`, namespaces, containers, cgroups, or KVM;
- memory overcommit, swap, demand paging, copy-on-write, userfaultfd, or exact
  Linux virtual address layouts;
- privileged applications that directly depend on Linux `/proc`, `/sys`, or
  device-specific ioctls until their compatibility services are implemented;
- security isolation on profiles labeled `unprotected`.

An exclusion MAY later be removed only with behavioral tests.

## 11. C++26 and runtime requirements

- All C++ translation units MUST compile with the Conda environment's
  `clang++`, `-std=c++2c`, `-fno-exceptions`, and `-fno-rtti`.
- The build MUST fail if the selected compiler is not Clang or does not satisfy
  the recorded minimum version and feature probes.
- Architecture assembly MUST be minimal and wrapped by typed C++ interfaces.
- Applicable C++26 features SHOULD be used when they simplify correctness;
  novelty alone is not a reason to use a feature.
- The kernel MUST NOT depend on hosted startup, locale, environment variables,
  filesystem-backed standard streams, thread-local runtime allocation, or
  implicit global constructors.
- The kernel MUST NOT throw, catch, unwind, or link an exception unwinder.
  Build and binary-inspection tests MUST enforce this.
- Fallible operations MUST return an explicit status, `std::expected`, or a
  project result type.
- Kernel panic is a non-returning crash operation, not an exception.
- Formatting strings SHOULD be compile-time checked.
- Host tools and user-space services SHOULD use `std::format`.
- Kernel formatting MUST be bounded and non-throwing. Direct `std::format` use
  in the kernel is prohibited until a toolchain probe plus fault-injection test
  proves that allocation and format failures cannot throw or unwind.
- Floating point, SIMD state, dynamic initialization, heap use, and thread-local
  storage are forbidden in the kernel until individually enabled by an
  architecture decision record and tests.

## 12. Build and repository requirements

- The repository MUST provide one documented environment activation command,
  currently `export PATH="$PWD/.conda/bin:$PATH"`.
- Builds MUST be reproducible from a clean tree without using the host compiler
  by accident.
- Build configurations MUST include host tests and each target profile.
- Generated files MUST record their source version and be reproducibly
  regenerated.
- Linux-derived UAPI material MUST retain its applicable SPDX identifier and
  notices. Linux implementation code is a behavioral reference and MUST NOT be
  copied into differently licensed Mikos code without an explicit licensing
  decision and provenance record.
- Warnings selected by the project are errors in CI.
- Linker maps and machine-readable binary inspection reports MUST be retained as
  test artifacts.
- Kernel binaries MUST be checked for forbidden symbols, unwind sections,
  unexpected constructors, dynamic dependencies, and architecture violations.
- A debug build MAY contain additional assertions and tracing, but observable
  ABI behavior MUST match release builds.

## 13. Test-driven development requirements

Every change follows red-green-refactor:

1. add or update a failing test that states the desired behavior;
2. implement the smallest behavior that passes it;
3. refactor while all tests remain green.

Required test layers:

- **host unit tests:** pure algorithms, parsers, region validation, queues,
  syscall metadata, and service state machines;
- **compile tests:** concepts, ABI layouts, feature probes, forbidden language
  features, and cross-target headers;
- **model/property tests:** queue interleavings, integer/range overflow,
  allocators, handles, and syscall argument decoding;
- **target unit tests:** freestanding code executed in the target environment;
- **boot tests:** serial/semihosted machine-readable boot protocol with timeout;
- **integration tests:** user program to syscall to service and back;
- **conformance tests:** Linux ABI fixtures, libc tests, and applicable LTP cases;
- **fault-injection tests:** exhaustion, malformed pointers/images/messages,
  queue overflow, unexpected IRQ, and service crashes;
- **performance tests:** event budget, syscall/IPC cost, boot time, and memory
  footprint with recorded baselines;
- **hardware tests:** protection and platform behavior not faithfully modeled by
  QEMU.

Tests MUST be deterministic by default, have bounded run time, and leave enough
diagnostic state to reproduce failures. A flaky test is a defect, not an
acceptable retry condition.

## 14. Observability and diagnostics

- Kernel tracing MUST use a bounded preallocated ring with fixed-size records.
- Formatting and I/O happen outside critical kernel paths where practical.
- Tracing MUST be safe when user services are absent or broken.
- Counters MUST cover queue depth/overflow, event-cycle budget, syscalls by
  result, validation failures, unexpected IRQs, and service timeouts.
- Test output MUST have a stable machine-readable result protocol.
- No diagnostic path may block indefinitely or allocate from an unbounded heap.

## 15. Security and robustness

- All lengths, offsets, counts, and address arithmetic crossing the ABI boundary
  MUST be overflow checked.
- Handles and capabilities MUST carry generation or equivalent stale-reference
  protection.
- IPC endpoints MUST enforce ownership and message-size bounds.
- DMA-capable services MUST be constrained to explicitly granted physical
  regions where hardware permits.
- Secrets and freed kernel objects SHOULD be cleared when ownership changes.
- Release builds MUST have stack protection and control-flow hardening where
  compatible with the freestanding runtime and verified target support.
- Unsupported security properties MUST be disclosed per architecture profile.

## 16. Release acceptance

A milestone release is acceptable only when:

- its supported architecture profiles boot through the automated harness;
- all required tests pass with zero unexplained skips;
- the generated syscall manifest matches the pinned Linux UAPI source;
- the compatibility report lists every syscall and test-suite result;
- binary inspection finds no exceptions, unwinding, RTTI, forbidden dynamic
  dependencies, or accidental page-table code in strict profiles;
- all ordinary IRQ sources are confirmed disabled, and the sole scheduling
  timer is confirmed unable to interrupt kernel execution;
- memory protection claims have negative-access tests;
- known limitations and architecture decisions are current.
