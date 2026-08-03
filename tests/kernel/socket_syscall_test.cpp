#include <mikos/abi/riscv32.hpp>
#include <mikos/abi/socket.hpp>

#include <support/test.hpp>

int main() {
  mikos::test::Suite suite{"kernel/socket_syscall"};
  using mikos::abi::socket::ValidationResult;

  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::socket) == 198);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(2, 2, 0) ==
                         ValidationResult::success);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(2, 1, 0) ==
                         ValidationResult::success);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(2, 1, 6) ==
                         ValidationResult::success);
  MIKOS_CHECK(
      suite,
      mikos::abi::socket::validate(
          2, 2 | mikos::abi::socket::sock_nonblock |
                 mikos::abi::socket::sock_cloexec,
          0) == ValidationResult::success);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(10, 2, 0) ==
                         ValidationResult::address_family_not_supported);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(2, 3, 0) ==
                         ValidationResult::socket_type_not_supported);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(2, 2, 17) ==
                         ValidationResult::protocol_not_supported);
  MIKOS_CHECK(suite, mikos::abi::socket::validate(2, 1, 17) ==
                         ValidationResult::protocol_not_supported);
  MIKOS_CHECK(suite, sizeof(mikos::abi::socket::SockaddrIn) == 16);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::bind) == 200);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::listen) == 201);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::accept) == 202);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::accept4) == 242);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::pselect6) == 72);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(mikos::abi::riscv32::Syscall::ppoll) == 73);
  MIKOS_CHECK(
      suite,
      static_cast<mikos::u32>(
          mikos::abi::riscv32::Syscall::pselect6_time64) == 413);

  return suite.finish();
}
