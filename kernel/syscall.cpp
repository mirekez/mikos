#include <mikos/arch.hpp>
#include <mikos/kernel.hpp>
#include <mikos/abi/riscv32.hpp>

extern "C" void* memset(void*, int, mikos::usize);
extern "C" void* memcpy(void*, const void*, mikos::usize);

namespace mikos {
namespace {

using abi::riscv32::Errno;
using abi::riscv32::Syscall;
using abi::riscv32::error;

struct [[gnu::packed]] Iovec32 {
  u32 base;
  u32 size;
};

struct [[gnu::packed]] Rlimit64 {
  u64 current;
  u64 maximum;
};

struct [[gnu::packed]] Timespec32 {
  i32 seconds;
  i32 nanoseconds;
};

struct [[gnu::packed]] Timespec64 {
  u64 seconds;
  u64 nanoseconds;
};

struct [[gnu::packed]] Timeval32 {
  i32 seconds;
  i32 microseconds;
};

struct TickTime {
  u64 seconds;
  u32 nanoseconds;
};

[[nodiscard]] TickTime tick_time() {
  constexpr u32 ticks_per_second = 10'000'000;
  const u64 ticks = arch::time_ticks();
  const u32 low = static_cast<u32>(ticks);
  const u32 high = static_cast<u32>(ticks >> 32);
  u32 quotient_low = 0;
  u32 quotient_high = 0;
  u32 remainder = 0;
  for (u32 bit = 64; bit != 0; --bit) {
    const u32 source = bit > 32 ? high >> (bit - 33) : low >> (bit - 1);
    remainder = (remainder << 1) | (source & 1u);
    if (remainder >= ticks_per_second) {
      remainder -= ticks_per_second;
      if (bit > 32) {
        quotient_high |= 1u << (bit - 33);
      } else {
        quotient_low |= 1u << (bit - 1);
      }
    }
  }
  return TickTime{(static_cast<u64>(quotient_high) << 32) | quotient_low,
                  remainder * 100};
}

[[nodiscard]] bool user_string_is(u32 address, const char* expected) {
  for (u32 i = 0; i < 64; ++i) {
    if (address > user_end - i || !user_memory.contains(address + i, 1)) {
      return false;
    }
    const char actual = *reinterpret_cast<const char*>(address + i);
    if (actual != expected[i]) {
      return false;
    }
    if (actual == '\0') {
      return true;
    }
  }
  return false;
}

[[nodiscard]] i32 clock_gettime(u32 address, bool time64) {
  const auto time = tick_time();
  if (time64) {
    if (!user_memory.contains(address, sizeof(Timespec64))) {
      return error(Errno::bad_address);
    }
    const Timespec64 value{time.seconds, time.nanoseconds};
    memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  } else {
    if (!user_memory.contains(address, sizeof(Timespec32))) {
      return error(Errno::bad_address);
    }
    const Timespec32 value{static_cast<i32>(time.seconds),
                           static_cast<i32>(time.nanoseconds)};
    memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  }
  return 0;
}

[[nodiscard]] i32 gettimeofday(u32 address) {
  if (address == 0) {
    return 0;
  }
  if (!user_memory.contains(address, sizeof(Timeval32))) {
    return error(Errno::bad_address);
  }
  const auto time = tick_time();
  const Timeval32 value{
      static_cast<i32>(time.seconds),
      static_cast<i32>(time.nanoseconds / 1000)};
  memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  return 0;
}

[[nodiscard]] i32 uname(u32 address) {
  constexpr u32 field_size = 65;
  constexpr u32 field_count = 6;
  constexpr u32 size = field_size * field_count;
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  auto* output = reinterpret_cast<char*>(address);
  memset(output, 0, size);
  constexpr const char* fields[field_count] = {
      "Mikos", "mikos", "0.1", "rv32-flat", "riscv32", ""};
  for (u32 field = 0; field < field_count; ++field) {
    for (u32 i = 0; fields[field][i] != '\0'; ++i) {
      output[field * field_size + i] = fields[field][i];
    }
  }
  return 0;
}

[[nodiscard]] i32 sysinfo(u32 address) {
  constexpr u32 size = 64;
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  auto* words = reinterpret_cast<u32*>(address);
  memset(words, 0, size);
  words[0] = static_cast<u32>(tick_time().seconds);
  words[4] = 16 * 1024 * 1024;
  words[5] = 4 * 1024 * 1024;
  *reinterpret_cast<u16*>(address + 40) = 1;
  words[13] = 1;
  return 0;
}

[[nodiscard]] i32 getcpu(u32 cpu, u32 node) {
  if (cpu != 0) {
    if (!user_memory.contains(cpu, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u32*>(cpu) = 0;
  }
  if (node != 0) {
    if (!user_memory.contains(node, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u32*>(node) = 0;
  }
  return 0;
}

[[nodiscard]] i32 write(u32 descriptor, u32 address, u32 size) {
  if (descriptor != 1 && descriptor != 2) {
    return error(Errno::bad_file_descriptor);
  }
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  uart_write(reinterpret_cast<const char*>(address), size);
  return static_cast<i32>(size);
}

[[nodiscard]] i32 writev(u32 descriptor, u32 address, u32 count) {
  if (count > 1024 ||
      !user_memory.contains(address, count * sizeof(Iovec32)) ||
      !user_memory.aligned(address, alignof(Iovec32))) {
    return error(Errno::bad_address);
  }
  const auto* vectors = reinterpret_cast<const Iovec32*>(address);
  u32 total = 0;
  for (u32 i = 0; i < count; ++i) {
    const i32 result = write(descriptor, vectors[i].base, vectors[i].size);
    if (result < 0) {
      return result;
    }
    if (vectors[i].size > 0x7fffffffu - total) {
      return error(Errno::invalid_argument);
    }
    total += vectors[i].size;
  }
  return static_cast<i32>(total);
}

[[nodiscard]] i32 read(u32 descriptor, u32 address, u32 size) {
  if (descriptor != 0) {
    return error(Errno::bad_file_descriptor);
  }
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  if (size == 0) {
    return 0;
  }
  constexpr u32 uart_base = 0x82000000;
  auto& data = *reinterpret_cast<volatile u8*>(uart_base);
  auto& status = *reinterpret_cast<volatile u8*>(uart_base + 5);
  while ((status & 1u) == 0) {
  }
  auto* output = reinterpret_cast<u8*>(address);
  output[0] = data;
  u32 count = 1;
  while (count < size && (status & 1u) != 0) {
    output[count++] = data;
  }
  return static_cast<i32>(count);
}

[[nodiscard]] i32 getrandom(u32 address, u32 size) {
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  static u32 state = 0x6d696b6f;
  auto* output = reinterpret_cast<u8*>(address);
  for (u32 i = 0; i < size; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    output[i] = static_cast<u8>(state);
  }
  return static_cast<i32>(size);
}

[[nodiscard]] i32 mmap(u32 address, u32 size, u32 flags) {
  constexpr u32 map_fixed = 0x10;
  if (size == 0) {
    return error(Errno::invalid_argument);
  }
  const u32 rounded = align_up(size, static_cast<u32>(4096));
  if (rounded < size) {
    return error(Errno::no_memory);
  }
  if ((flags & map_fixed) != 0) {
    if (!user_memory.contains(address, rounded)) {
      return error(Errno::no_memory);
    }
    return static_cast<i32>(address);
  }
  const u32 result = process.mmap_cursor;
  if (!user_memory.contains(result, rounded) ||
      result >= user_stack_top ||
      rounded > user_stack_top - result) {
    return error(Errno::no_memory);
  }
  process.mmap_cursor += rounded;
  memset(reinterpret_cast<void*>(result), 0, rounded);
  return static_cast<i32>(result);
}

[[nodiscard]] i32 prlimit(u32 old_limit) {
  if (old_limit == 0) {
    return 0;
  }
  if (!user_memory.contains(old_limit, sizeof(Rlimit64))) {
    return error(Errno::bad_address);
  }
  constexpr Rlimit64 unlimited{~u64{0}, ~u64{0}};
  memcpy(reinterpret_cast<void*>(old_limit), &unlimited, sizeof(unlimited));
  return 0;
}

[[nodiscard]] i32 protect_or_unmap(u32 address, u32 size) {
  return user_memory.contains(address, size) ? 0 : error(Errno::no_memory);
}

[[nodiscard]] i32 unknown(u32 number) {
  write_text("MIKOS:ENOSYS ");
  write_u32(number);
  write_text(" ");
  write_text(abi::riscv32::name(number));
  write_text("\n");
  return error(Errno::no_syscall);
}

}  // namespace

i32 dispatch_syscall(TrapFrame& frame) {
  static u32 announced_image = ~u32{0};
  if (announced_image != process.image) {
    write_text(process.image == 0 ? "MIKOS:BUSYBOX_ENTRY\n"
                                  : "MIKOS:STRESS_NG_ENTRY\n");
    announced_image = process.image;
  }
  const u32 number = frame.x[17];
  const u32 a0 = frame.x[10];
  const u32 a1 = frame.x[11];
  const u32 a2 = frame.x[12];
  const u32 a3 = frame.x[13];
  switch (static_cast<Syscall>(number)) {
    case Syscall::write:
      return write(a0, a1, a2);
    case Syscall::writev:
      return writev(a0, a1, a2);
    case Syscall::read:
      return read(a0, a1, a2);
    case Syscall::faccessat:
      return user_string_is(a1, ".") ? 0 : error(Errno::no_entry);
    case Syscall::openat:
      return error(Errno::no_entry);
    case Syscall::exit:
    case Syscall::exit_group:
      if (process.image == 0) {
        write_text("MIKOS:BUSYBOX_EXIT ");
        write_u32(a0);
        write_text("\n");
        if (a0 != 0) {
          shutdown(a0);
        }
        start_stress_ng(frame);
        return 0;
      }
      if (a0 == 0) {
        write_text("MIKOS:STRESS_NG_PASS\n");
      }
      write_text("MIKOS:PREEMPTIONS ");
      write_u32(scheduler.user_preemptions);
      write_text("\n");
      write_text("MIKOS:TIMER_CONTRACT_VIOLATIONS ");
      write_u32(scheduler.contract_violations);
      write_text("\n");
      write_text("MIKOS:EXIT ");
      write_u32(a0);
      write_text("\n");
      shutdown(a0);
    case Syscall::set_tid_address:
      return 1;
    case Syscall::set_robust_list:
      return 0;
    case Syscall::rseq:
      return error(Errno::no_syscall);
    case Syscall::getpid:
    case Syscall::gettid:
      return 1;
    case Syscall::getppid:
      return 0;
    case Syscall::getuid:
    case Syscall::geteuid:
    case Syscall::getgid:
    case Syscall::getegid:
      return 0;
    case Syscall::brk:
      if (a0 == 0) {
        return static_cast<i32>(process.brk);
      }
      if (a0 >= user_begin && a0 < process.mmap_cursor) {
        process.brk = a0;
      }
      return static_cast<i32>(process.brk);
    case Syscall::mmap2:
      return mmap(a0, a1, a3);
    case Syscall::munmap:
    case Syscall::mprotect:
      return protect_or_unmap(a0, a1);
    case Syscall::riscv_hwprobe:
      return error(Errno::no_syscall);
    case Syscall::getrandom:
      return getrandom(a0, a1);
    case Syscall::clock_gettime32:
      return clock_gettime(a1, false);
    case Syscall::clock_gettime64:
      return clock_gettime(a1, true);
    case Syscall::gettimeofday:
      return gettimeofday(a0);
    case Syscall::uname:
      return uname(a0);
    case Syscall::getcpu:
      return getcpu(a0, a1);
    case Syscall::sysinfo:
      return sysinfo(a0);
    case Syscall::getrusage:
      if (!user_memory.contains(a1, 72)) {
        return error(Errno::bad_address);
      }
      memset(reinterpret_cast<void*>(a1), 0, 72);
      return 0;
    case Syscall::sched_getaffinity:
      if (a1 == 0 || !user_memory.contains(a2, a1)) {
        return error(Errno::bad_address);
      }
      memset(reinterpret_cast<void*>(a2), 0, a1);
      *reinterpret_cast<u8*>(a2) = 1;
      return 4;
    case Syscall::prlimit64:
      return prlimit(a3);
    case Syscall::ioctl:
      return error(Errno::not_a_tty);
    case Syscall::rt_sigaction:
    case Syscall::rt_sigprocmask:
    case Syscall::sigaltstack:
      return 0;
    case Syscall::statfs64:
    case Syscall::readlinkat:
    case Syscall::fstatat64:
    case Syscall::fstat64:
      return unknown(number);
  }
  return unknown(number);
}

}  // namespace mikos
