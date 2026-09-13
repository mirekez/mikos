# Kernel C++ containers

Include `<mikos/container/standard.hpp>` in kernel code. The build prepares
checksum-pinned sources automatically under `build/kernel-cxx`; nothing from
the host's C++ library is linked. First preparation needs curl, tar, CMake,
flock and network access. Archives cached there support subsequent offline
builds. Set `RISCV_HOME` (default `~/riscv`) to an RV32 Newlib or Linux
toolchain installation, or override `RISCV_PREFIX` / `RISCV_SYSROOT`.

| Name | Implementation | Allocation |
|---|---|---|
| `mikos::vector<T>` | `std::vector` | Explicit kernel allocator |
| `mikos::list<T>` | `std::list` | Explicit kernel allocator |
| `mikos::map<K,V,Compare>` | `std::map` | Explicit kernel allocator |
| `mikos::unordered_set<K,Hash,Equal>` | `std::unordered_set` | Explicit kernel allocator |
| `mikos::inplace_vector<T,N>` | Beman fallback; future `std::inplace_vector` | Embedded storage |
| `std::array`, `std::span`, `std::string_view` | libc++ | No allocation |

The aliases retain the full underlying standard container API. For spelling
`std::vector` directly, provide `mikos::memory::KernelAllocator<T>` as its
allocator template argument. Default `std::allocator` allocations fail to link
deliberately; kernel code must state who owns the memory.

```cpp
#include <mikos/container/standard.hpp>

bool example() {
  alignas(64) unsigned char bytes[4096]; // choose a subsystem budget
  mikos::memory::Arena arena;
  if (!arena.initialize(bytes, sizeof(bytes))) return false;
  mikos::memory::Resource resource(arena);
  mikos::vector<unsigned> values{
      mikos::memory::KernelAllocator<unsigned>{resource}};
  if (!mikos::try_reserve(values, 64)) return false; // map to ENOMEM if appropriate
  values.push_back(42); // safe while size() < capacity()

  mikos::unordered_set<unsigned> keys{
      mikos::memory::KernelAllocator<unsigned>{resource}};
  if (!mikos::try_insert(keys, 42u)) return false;
  mikos::inplace_vector<unsigned, 8> local;
  return local.try_push_back(values.front()) != nullptr;
} // containers die before resource, arena and storage
```

Checked helpers: `try_reserve(vector, n)`, `try_reserve(unordered_set, n)`,
`try_push_back(list, value)`, `try_insert(map, key, value)` and
`try_insert(unordered_set, key)`. Insert returns true for an existing key and
leaves its value unchanged. Helpers admit trivially copyable values only
(excluding the packed `vector<bool>` specialization); hash
and comparison callbacks must not allocate or reenter the resource. Set helpers
require `max_load_factor() == 1`. A false result preserves existing elements;
set bucket capacity may already have grown before node exhaustion.

Direct `reserve`, `emplace`, copying, and other allocating standard methods are
available, including nontrivial element types, but exhaustion fails-stop. A
custom allocator cannot make an arbitrary standard operation return `ENOMEM`.
`inplace_vector`'s checked insertion works with nontrivial elements and does not
allocate. Prefer it for small bounded collections. Do not allocate in timers,
do not share an arena across harts without external serialization, and do not
memcpy owning container objects during process bookkeeping. TCP packet slots
use `std::array<std::optional<Entry>, 16>`: stable addresses, bounded scans and
no allocation on the packet path.

`Arena` performs first-fit allocation and adjacent free-block coalescing in
bounded caller storage. It supports over-alignment, rejects size overflow, and
reports live allocation count, occupied bytes (including headers/padding), and
high-water use. Allocation/free cost is linear in the arena's block count.
This is a minimal allocator adapter; optimized size-class pools remain future
work. No mmap or extra reserved heap region is introduced.

Run `make kernel-cxx-test`: host UBSan tests, all four RV32 kernel links and
binary inspection, then a disk-free QEMU test of all five containers inside the
real kernel. Every normal boot also emits `MIKOS:KERNEL_CXX_OK`. `make test`
includes the host tests. With the native Tribe simulator already built, the
same boot check can run without a disk, network peer or Verilator:

```sh
build/tests/tribe/cpphdl-build/tribe64/tribe64 --noveril \
  --program build/mikos-tribe-rv32.elf --elf --cycles 8000000 \
  --start-mem-addr 0x80000000 --ram-size 8388608 --boot-priv m \
  --expected-output-contains MIKOS:KERNEL_CXX_OK --mirror-uart
```

This checks container execution, not the full Tribe filesystem/SSH workflow.
To enable ASan on a compatible host, remove only the
generated standard_containers test executable and rebuild with
`make -C tests KERNEL_CXX_SANITIZERS=address,undefined kernel-cxx-check`.
This machine's system-preloaded monitoring agent uses RTLD_DEEPBIND and prevents
ASan from starting; UBSan and the target tests remain runnable.

The linker enforces a 500000-byte initialized kernel image. Inspection reports
resident RAM separately; BSS, stack, subsystem backing storage and process
snapshots are not hidden as image bytes. See
[ADR-0002](adr/0002-kernel-standard-containers.md) for the exact contract.

Source licenses remain in the downloaded trees: LLVM libc++/compiler-rt and
Beman inplace_vector use Apache-2.0 WITH LLVM-exception. Versions and SHA-256
hashes are recorded in `support/kernel-cxx/prepare.sh`. Headers are not copied
into the repository or modified; generated vendor configuration is separate.
