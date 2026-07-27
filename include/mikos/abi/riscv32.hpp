#pragma once

#include <mikos/base.hpp>

namespace mikos::abi::riscv32 {

// Linux 6.19 asm-generic/RV32 syscall numbers used by the current POC workloads.
// Unknown numbers are deliberately valid inputs to the dispatcher and return
// -ENOSYS.
enum class Syscall : u32 {
  ioctl = 29,
  statfs64 = 43,
  faccessat = 48,
  openat = 56,
  read = 63,
  write = 64,
  writev = 66,
  readlinkat = 78,
  fstatat64 = 79,
  fstat64 = 80,
  exit = 93,
  exit_group = 94,
  set_tid_address = 96,
  set_robust_list = 99,
  clock_gettime32 = 113,
  sched_getaffinity = 123,
  sigaltstack = 132,
  rt_sigaction = 134,
  rt_sigprocmask = 135,
  uname = 160,
  getrusage = 165,
  getcpu = 168,
  gettimeofday = 169,
  getpid = 172,
  getppid = 173,
  getuid = 174,
  geteuid = 175,
  getgid = 176,
  getegid = 177,
  gettid = 178,
  sysinfo = 179,
  brk = 214,
  munmap = 215,
  mmap2 = 222,
  mprotect = 226,
  riscv_hwprobe = 258,
  prlimit64 = 261,
  getrandom = 278,
  rseq = 293,
  clock_gettime64 = 403,
};

enum class Errno : i32 {
  no_entry = 2,
  bad_file_descriptor = 9,
  bad_address = 14,
  invalid_argument = 22,
  not_a_tty = 25,
  no_syscall = 38,
  no_memory = 12,
};

[[nodiscard]] constexpr i32 error(Errno value) {
  return -static_cast<i32>(value);
}

[[nodiscard]] constexpr const char* name(u32 number) {
  switch (static_cast<Syscall>(number)) {
    case Syscall::ioctl:
      return "ioctl";
    case Syscall::statfs64:
      return "statfs64";
    case Syscall::faccessat:
      return "faccessat";
    case Syscall::openat:
      return "openat";
    case Syscall::read:
      return "read";
    case Syscall::write:
      return "write";
    case Syscall::writev:
      return "writev";
    case Syscall::exit:
      return "exit";
    case Syscall::exit_group:
      return "exit_group";
    case Syscall::set_tid_address:
      return "set_tid_address";
    case Syscall::set_robust_list:
      return "set_robust_list";
    case Syscall::uname:
      return "uname";
    case Syscall::getrusage:
      return "getrusage";
    case Syscall::getcpu:
      return "getcpu";
    case Syscall::gettimeofday:
      return "gettimeofday";
    case Syscall::getpid:
      return "getpid";
    case Syscall::getppid:
      return "getppid";
    case Syscall::getuid:
      return "getuid";
    case Syscall::geteuid:
      return "geteuid";
    case Syscall::getgid:
      return "getgid";
    case Syscall::getegid:
      return "getegid";
    case Syscall::gettid:
      return "gettid";
    case Syscall::sysinfo:
      return "sysinfo";
    case Syscall::brk:
      return "brk";
    case Syscall::munmap:
      return "munmap";
    case Syscall::mmap2:
      return "mmap2";
    case Syscall::mprotect:
      return "mprotect";
    case Syscall::riscv_hwprobe:
      return "riscv_hwprobe";
    case Syscall::prlimit64:
      return "prlimit64";
    case Syscall::getrandom:
      return "getrandom";
    case Syscall::rseq:
      return "rseq";
    case Syscall::clock_gettime32:
      return "clock_gettime32";
    case Syscall::clock_gettime64:
      return "clock_gettime64";
    case Syscall::sched_getaffinity:
      return "sched_getaffinity";
    case Syscall::sigaltstack:
      return "sigaltstack";
    case Syscall::readlinkat:
      return "readlinkat";
    case Syscall::fstatat64:
      return "fstatat64";
    case Syscall::fstat64:
      return "fstat64";
    case Syscall::rt_sigaction:
      return "rt_sigaction";
    case Syscall::rt_sigprocmask:
      return "rt_sigprocmask";
  }
  return "unknown";
}

}  // namespace mikos::abi::riscv32
