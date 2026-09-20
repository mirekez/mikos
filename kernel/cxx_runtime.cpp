#include <mikos/kernel.hpp>
#include <mikos/memory/arena.hpp>
#include <__verbose_abort>
#include <cstdlib>
#include <new>

namespace mikos::memory {
[[noreturn]] void allocation_failure() {
  write_text("MIKOS:KERNEL_ALLOCATION_EXHAUSTED\n");
  shutdown(30);
}
[[noreturn]] void allocation_contract_failure() {
  write_text("MIKOS:KERNEL_ALLOCATION_CONTRACT\n");
  shutdown(31);
}
}

// No global allocating new/delete: dynamic containers must carry an explicit
// kernel allocator. Accidental std::allocator use fails the final link.
// Match the C library declaration brought in by the standard containers.
// Newlib and glibc use different exception specifications for abort().
extern "C" void abort() noexcept(noexcept(std::abort())) {
  mikos::memory::allocation_contract_failure();
}
extern "C" [[noreturn]] void __assert_fail(const char*, const char*, unsigned,
                                          const char*) noexcept {
  mikos::memory::allocation_contract_failure();
}
_LIBCPP_BEGIN_NAMESPACE_STD
[[noreturn]] void __libcpp_verbose_abort(const char*, ...) noexcept {
  mikos::memory::allocation_contract_failure();
}
_LIBCPP_END_NAMESPACE_STD
namespace std {
[[noreturn]] void __throw_bad_alloc() { mikos::memory::allocation_failure(); }
}
