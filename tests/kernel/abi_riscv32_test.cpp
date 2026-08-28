#include <string>

#include <mikos/abi/riscv32.hpp>
#include <mikos/base.hpp>

#include <support/test.hpp>

int main() {
  mikos::test::Suite suite{"kernel/abi_riscv32"};
  using mikos::abi::riscv32::Errno;
  using mikos::abi::riscv32::Syscall;

  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::mkdirat) == 34);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::fchmodat) == 53);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::pipe2) == 59);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::write) == 64);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::exit_group) == 94);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::waitid) == 95);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::umask) == 166);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::kill) == 129);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::rt_sigreturn) == 139);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::setsid) == 157);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::brk) == 214);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::clone) == 220);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::execve) == 221);
  MIKOS_CHECK(suite, static_cast<mikos::i32>(Errno::text_file_busy) == 26);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::wait4) == 260);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::statx) == 291);
  MIKOS_CHECK(suite,
              static_cast<mikos::u32>(Syscall::clock_gettime64) == 403);
  MIKOS_CHECK(suite, static_cast<mikos::u32>(Syscall::ppoll64) == 414);
  MIKOS_CHECK(suite,
              mikos::abi::riscv32::error(Errno::no_syscall) == -38);
  MIKOS_CHECK(suite,
              std::string{mikos::abi::riscv32::name(64)} == "write");
  MIKOS_CHECK(suite,
              std::string{mikos::abi::riscv32::name(34)} == "mkdirat");
  MIKOS_CHECK(suite,
              std::string{mikos::abi::riscv32::name(53)} == "fchmodat");
  MIKOS_CHECK(
      suite, std::string{mikos::abi::riscv32::name(123)} ==
                 "sched_getaffinity");
  MIKOS_CHECK(suite,
              std::string{mikos::abi::riscv32::name(166)} == "umask");
  MIKOS_CHECK(suite,
              std::string{mikos::abi::riscv32::name(0xffffffff)} ==
                  "unknown");

  return suite.finish();
}
