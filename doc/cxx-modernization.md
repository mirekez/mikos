# Kernel and driver C++ cleanup

The implementation uses standard value types where they remove bookkeeping,
and ownership classes where they protect a lifetime invariant. It does not
replace device protocols with general-purpose containers or add virtual dispatch.

- Filesystem `Result<T>` is an alias for `std::expected<T, Error>`. Errors do
  not construct dummy volumes/nodes; consumers check before dereferencing.
- FAT32 name/slot builders use bounded `inplace_vector`; the LFN size limit is
  measured in UTF-16 units, not UTF-8 bytes. On-disk encoding remains explicit.
- ext4 scratch blocks and the aligned sector cache use `std::array`.
  SectorReader offers `std::span` overloads without changing device I/O records.
- Names expose `string_view`; standard algorithms perform comparisons and
  copying. Owning name storage and FAT case-folding policy remain explicit.
- RootFilesystem owns the block device, cache, volume and readiness together.
  It cannot be copied; `constinit` prevents implicit boot-time constructors.
- The two TCP packet tables use arrays of optional entries. With sixteen slots
  each, lookup is a bounded linear scan, deletion never moves another payload,
  and neither allocation nor rehashing is needed. The old generic hash table
  and its implementation-specific tombstone test are removed; protocol tests
  cover exhaustion, duplicate preservation, wraparound and reuse instead.
- Unreferenced `drivers/fs/memory.hpp` was removed, not replaced by another
  filesystem implementation. Deleted tracked sources remain recoverable in Git.

## ABI and writev

C++ types stay inside the kernel. `Iovec32` remains an eight-byte, plain Linux
RV32 record with compile-time layout checks. The syscall validates the user
range and snapshots its descriptor array before constructing an internal span.
No STL object, allocator pointer, vtable or kernel-owned address crosses the ABI.
This retains the existing flat-address-space validation model; it does not add
MMU isolation or make concurrent user-buffer mutation safe in general.

`writev` now preflights aggregate length, sums actual written bytes, stops after
a short write, and returns prior progress when a later write fails. Its bounded
descriptor snapshot uses at most 8 KiB of the existing 64 KiB kernel stack.

## Validation

- `make test`: existing driver/filesystem/network/process regressions plus
  scatter-write error/progress tests, FAT LFN boundary/Unicode cases, span
  boundaries and TCP table saturation/reuse tests.
- `make kernel-cxx-test`: all four RV32 builds, no hosted runtime/global
  constructors, initialized image under 500000 bytes, real-kernel C++ smoke.
- `make qemu-test qemu-net-test`: writable ext4/fsck/BusyBox/stress-ng and network.
- Native C++ Tribe smoke command: see [kernel-cxx.md](kernel-cxx.md).

Source reduction is not a guarantee of smaller machine code. Report initialized
image and resident RAM separately; keep the linker size gate enabled.
