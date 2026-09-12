# ADR-0002: bounded standard containers in the kernel

Status: accepted for RV32-flat.

Use pinned libc++ 21.1.3 headers and its hash-table support object with Clang.
Disable threads, exceptions, RTTI, locale, filesystem, wide characters and global
initialization. Keep fast hardening; failure reports a kernel diagnostic and
shuts down. Only the target toolchain's C declarations are used; no Linux libc,
libstdc++, libc++abi or unwinder is linked.

Permit `vector`, `list`, `map` and `unordered_set` with an explicit
`KernelAllocator<T>` backed by caller-owned, bounded physical memory. An arena
does not obtain RAM itself and must never overlap the fork-snapshot arena or
user/DMA regions. Use existing kernel-owned storage or a separately reserved
region. Allocation is single-owner, non-recursive and forbidden in timer paths.
No global allocating `new/delete` is supplied. Arenas, resources and containers
are explicitly initialized; their destruction order follows their ownership.
Container objects with ownership must never be byte-copied or zeroed.

Standard allocating operations do not support returning `ENOMEM`. `allocate`
never returns null: exhausted unchecked operations fail-stop. For recoverable
operations, use the checked reservation/insertion helpers, returning failure
before calling the standard operation. The initial helpers support trivially
copyable values and non-allocating hash/comparison callbacks. Their exact node
allocation contracts depend on the pinned libc++ and are tested on host and
RV32. Successful hash reallocation followed by node exhaustion can leave larger
bucket capacity but preserves the set's elements. This is an explicit, bounded
heap exception to REQUIREMENTS section 11, not permission for unrestricted heap
use or unwinding.

`inplace_vector` is exposed through `mikos::inplace_vector`, using the Beman
implementation pinned by commit until libc++ implements `std::inplace_vector`.
No replacement declarations are injected into `std`. Prefer `try_push_back` /
`try_emplace_back` for full-capacity recovery.

libc++ unordered containers calculate load factors with floating-point values.
Permit only their software binary32 arithmetic: five compiler-rt 21.1.3 leaf
sources and an integer-only ceiling helper. The RV32 ISA flags still exclude
F/D and no FPU state or libm is introduced. No floating-point environment/status
flags are provided. This is the narrow arithmetic exception to section 11.

The enforced 500 kB **image** budget is 500000 bytes from `__kernel_begin` to
`__kernel_image_end` (text, rodata, initialized data and alignment). Embedded
userspace and debug sections are not kernel code. Inspection also reports the
resident extent through `__kernel_end`, including BSS and the 64 KiB stack;
it is larger than 500 kB in the current implementation. Caller-owned arenas and
live fork snapshots must additionally be accounted for in RAM. This decision
does not claim a 500 kB total-RAM system.

Validation: `make kernel-cxx-test`, `make test`, and `make qemu-test`.
