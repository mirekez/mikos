#include <mikos/kernel.hpp>
#include <mikos/memory/arena.hpp>
#include <__verbose_abort>
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
extern "C" [[noreturn]] void abort() noexcept {
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
