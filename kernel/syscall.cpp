#include <mikos/arch.hpp>
#include <mikos/drivers/uart.hpp>
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

struct [[gnu::packed]] Pollfd32 {
  i32 descriptor;
  u16 events;
  u16 returned_events;
};

struct [[gnu::packed]] Timeval32 {
  i32 seconds;
  i32 microseconds;
};

struct TickTime {
  u64 seconds;
  u32 nanoseconds;
};

struct SuspendedParent {
  TrapFrame frame{};
  u32 brk{};
  u32 mmap_cursor{};
  u32 mutable_begin{};
  u32 stack_begin{};
  u32 mutable_size{};
  u32 mmap_size{};
  u32 stack_size{};
  u32 child_status{};
  bool cwd_proc{};
  bool active{};
  bool memory_saved{};
  bool child_waitable{};
};

SuspendedParent parent{};
inline constexpr u32 parent_backup_capacity = 4 * 1024 * 1024;
[[gnu::section(".noinit")]] alignas(16)
u8 parent_backup[parent_backup_capacity];

[[nodiscard]] bool user_string_is(u32 address, const char* expected);
void reset_descriptors();

[[nodiscard]] bool save_parent_memory() {
  if (parent.memory_saved) {
    return true;
  }
  if (parent.mutable_begin > parent.brk ||
      parent.mmap_cursor < 0x81800000 ||
      parent.stack_begin > user_stack_top) {
    return false;
  }
  parent.mutable_size = parent.brk - parent.mutable_begin;
  parent.mmap_size = parent.mmap_cursor - 0x81800000;
  parent.stack_size = user_stack_top - parent.stack_begin;
  const u64 total = static_cast<u64>(parent.mutable_size) +
                    parent.mmap_size + parent.stack_size;
  if (total > parent_backup_capacity) {
    return false;
  }
  u32 cursor = 0;
  memcpy(parent_backup + cursor,
         reinterpret_cast<const void*>(parent.mutable_begin),
         parent.mutable_size);
  cursor += parent.mutable_size;
  memcpy(parent_backup + cursor, reinterpret_cast<const void*>(0x81800000),
         parent.mmap_size);
  cursor += parent.mmap_size;
  memcpy(parent_backup + cursor,
         reinterpret_cast<const void*>(parent.stack_begin),
         parent.stack_size);
  parent.memory_saved = true;
  return true;
}

void restore_parent(TrapFrame& frame, u32 status) {
  if (parent.memory_saved) {
    u32 cursor = 0;
    memcpy(reinterpret_cast<void*>(parent.mutable_begin),
           parent_backup + cursor, parent.mutable_size);
    cursor += parent.mutable_size;
    memcpy(reinterpret_cast<void*>(0x81800000), parent_backup + cursor,
           parent.mmap_size);
    cursor += parent.mmap_size;
    memcpy(reinterpret_cast<void*>(parent.stack_begin), parent_backup + cursor,
           parent.stack_size);
  }
  frame = parent.frame;
  frame.x[10] = 2;
  frame.mepc += 4;
  process.brk = parent.brk;
  process.mmap_cursor = parent.mmap_cursor;
  process.mutable_begin = parent.mutable_begin;
  process.pid = 1;
  process.image = 0;
  process.cwd_proc = parent.cwd_proc;
  process.image_replaced = true;
  parent.child_status = status;
  parent.child_waitable = true;
  parent.active = false;
  parent.memory_saved = false;
  reset_descriptors();
}

[[nodiscard]] i32 clone(TrapFrame& frame, u32 flags, u32 stack) {
  constexpr u32 signal_mask = 0xff;
  constexpr u32 sigchld = 17;
  constexpr u32 clone_vm = 0x100;
  constexpr u32 clone_vfork = 0x4000;
  constexpr u32 supported = clone_vm | clone_vfork | sigchld;
  if ((flags & ~signal_mask) != (supported & ~signal_mask) ||
      (flags & signal_mask) != sigchld || parent.active ||
      !user_memory.contains(stack, 1)) {
    return error(Errno::invalid_argument);
  }
  parent.frame = frame;
  parent.brk = process.brk;
  parent.mmap_cursor = process.mmap_cursor;
  parent.mutable_begin = process.mutable_begin;
  parent.cwd_proc = process.cwd_proc;
  parent.stack_begin = align_down(stack, static_cast<u32>(16));
  parent.active = true;
  parent.memory_saved = false;
  parent.child_waitable = false;
  process.pid = 2;
  return 0;
}

[[nodiscard]] bool copy_user_string(u32 address, char* output, u32 capacity) {
  if (address == 0 || capacity == 0) {
    return false;
  }
  for (u32 i = 0; i < capacity; ++i) {
    if (!user_memory.contains(address + i, 1)) {
      return false;
    }
    output[i] = *reinterpret_cast<const char*>(address + i);
    if (output[i] == '\0') {
      return true;
    }
  }
  return false;
}

enum class Node : u8 {
  none,
  root_directory,
  proc_directory,
  cpu_stat,
  meminfo,
  loadavg,
  pid1_directory,
  pid2_directory,
  pid1_stat,
  pid2_stat,
  pid1_cmdline,
  pid2_cmdline,
};

struct Descriptor {
  Node node{};
  u32 offset{};
};

Descriptor descriptors[13]{};

void reset_descriptors() {
  for (auto& descriptor : descriptors) {
    descriptor = {};
  }
}

[[nodiscard]] bool text_is(const char* actual, const char* expected) {
  while (*actual == *expected) {
    if (*actual == '\0') {
      return true;
    }
    ++actual;
    ++expected;
  }
  return false;
}

[[nodiscard]] u32 text_size(const char* text) {
  u32 size = 0;
  while (text[size] != '\0') {
    ++size;
  }
  return size;
}

[[nodiscard]] Node path_node(u32 address) {
  char path[64]{};
  if (!copy_user_string(address, path, sizeof(path))) {
    return Node::none;
  }
  if (text_is(path, "/")) {
    return Node::root_directory;
  }
  if (text_is(path, ".")) {
    return process.cwd_proc ? Node::proc_directory : Node::root_directory;
  }
  if (text_is(path, "/proc") || text_is(path, "/proc/") ||
      text_is(path, "./proc")) {
    return Node::proc_directory;
  }
  if (text_is(path, "/proc/1") || text_is(path, "/proc/1/")) {
    return Node::pid1_directory;
  }
  if (text_is(path, "/proc/2") || text_is(path, "/proc/2/")) {
    return Node::pid2_directory;
  }
  if ((process.cwd_proc && text_is(path, "stat")) ||
      text_is(path, "/proc/stat")) {
    return Node::cpu_stat;
  }
  if ((process.cwd_proc && text_is(path, "meminfo")) ||
      text_is(path, "/proc/meminfo")) {
    return Node::meminfo;
  }
  if ((process.cwd_proc && text_is(path, "loadavg")) ||
      text_is(path, "/proc/loadavg")) {
    return Node::loadavg;
  }
  if (text_is(path, "/proc/1/stat")) {
    return Node::pid1_stat;
  }
  if (text_is(path, "/proc/2/stat")) {
    return Node::pid2_stat;
  }
  if (text_is(path, "/proc/1/cmdline")) {
    return Node::pid1_cmdline;
  }
  if (text_is(path, "/proc/2/cmdline")) {
    return Node::pid2_cmdline;
  }
  return Node::none;
}

[[nodiscard]] bool directory(Node node) {
  return node == Node::root_directory || node == Node::proc_directory ||
         node == Node::pid1_directory || node == Node::pid2_directory;
}

[[maybe_unused, nodiscard]] i32 openat(u32 path) {
  const Node node = path_node(path);
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  for (u32 descriptor = 3; descriptor < 16; ++descriptor) {
    auto& slot = descriptors[descriptor - 3];
    if (slot.node == Node::none) {
      slot = Descriptor{node, 0};
      return static_cast<i32>(descriptor);
    }
  }
  return error(Errno::no_memory);
}

[[maybe_unused, nodiscard]] i32 close(u32 descriptor) {
  if (descriptor < 3 || descriptor >= 16 ||
      descriptors[descriptor - 3].node == Node::none) {
    return error(Errno::bad_file_descriptor);
  }
  descriptors[descriptor - 3] = {};
  return 0;
}

[[nodiscard]] const char* node_contents(Node node) {
  static constexpr const char cpu[] =
      "cpu  100 0 100 10000 0 0 0 0\n"
      "cpu0 100 0 100 10000 0 0 0 0\n";
  static constexpr const char memory[] =
      "MemTotal:       16384 kB\n"
      "MemFree:         4096 kB\n"
      "MemShared:          0 kB\n"
      "Buffers:          128 kB\n"
      "Cached:          2048 kB\n"
      "SReclaimable:       0 kB\n"
      "Shmem:              0 kB\n"
      "SwapTotal:          0 kB\n"
      "SwapFree:           0 kB\n";
  static constexpr const char load[] = "0.00 0.00 0.00 1/2 2\n";
  static constexpr const char shell_stat[] =
      "1 (sh) S 0 1 1 0 0 0 0 0 0 0 1 1 0 0 20 0 1 0 1 2500000 128 "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  static constexpr const char top_stat[] =
      "2 (top) R 1 2 2 0 0 0 0 0 0 0 2 1 0 0 20 0 1 0 1 2600000 144 "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  static constexpr const char shell_command[] = "sh\0";
  static constexpr const char top_command[] = "top\0";
  switch (node) {
    case Node::cpu_stat:
      return cpu;
    case Node::meminfo:
      return memory;
    case Node::loadavg:
      return load;
    case Node::pid1_stat:
      return shell_stat;
    case Node::pid2_stat:
      return top_stat;
    case Node::pid1_cmdline:
      return shell_command;
    case Node::pid2_cmdline:
      return top_command;
    default:
      return nullptr;
  }
}

[[nodiscard]] u32 node_size(Node node) {
  if (node == Node::pid1_cmdline) {
    return 3;
  }
  if (node == Node::pid2_cmdline) {
    return 4;
  }
  const char* contents = node_contents(node);
  return contents == nullptr ? 0 : text_size(contents);
}

[[nodiscard]] i32 read_virtual(u32 descriptor, u32 address, u32 size) {
  if (descriptor < 3 || descriptor >= 16) {
    return error(Errno::bad_file_descriptor);
  }
  auto& slot = descriptors[descriptor - 3];
  const char* contents = node_contents(slot.node);
  if (contents == nullptr) {
    return directory(slot.node) ? error(Errno::bad_file_descriptor)
                                : error(Errno::bad_file_descriptor);
  }
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  const u32 available = node_size(slot.node);
  if (slot.offset >= available) {
    return 0;
  }
  u32 count = available - slot.offset;
  if (count > size) {
    count = size;
  }
  memcpy(reinterpret_cast<void*>(address), contents + slot.offset, count);
  slot.offset += count;
  return static_cast<i32>(count);
}

[[nodiscard]] i32 emit_dirent(u32 address, u32 capacity, u64 inode,
                              u64 offset, u8 type, const char* name) {
  const u32 name_size = text_size(name) + 1;
  const u32 record_size = align_up(19 + name_size, static_cast<u32>(8));
  if (record_size > capacity) {
    return 0;
  }
  memset(reinterpret_cast<void*>(address), 0, record_size);
  memcpy(reinterpret_cast<void*>(address), &inode, sizeof(inode));
  memcpy(reinterpret_cast<void*>(address + 8), &offset, sizeof(offset));
  const u16 record_size16 = static_cast<u16>(record_size);
  memcpy(reinterpret_cast<void*>(address + 16), &record_size16,
         sizeof(record_size16));
  *reinterpret_cast<u8*>(address + 18) = type;
  memcpy(reinterpret_cast<void*>(address + 19), name, name_size);
  return static_cast<i32>(record_size);
}

[[maybe_unused, nodiscard]] i32 getdents64(u32 descriptor, u32 address,
                                          u32 size) {
  if (descriptor < 3 || descriptor >= 16 ||
      !user_memory.contains(address, size)) {
    return error(Errno::bad_file_descriptor);
  }
  auto& slot = descriptors[descriptor - 3];
  if (!directory(slot.node)) {
    return error(Errno::bad_file_descriptor);
  }
  constexpr u8 directory_type = 4;
  constexpr const char* root_entries[] = {".", "..", "proc"};
  constexpr const char* proc_entries[] = {".", "..", "1", "2"};
  const char* const* entries = root_entries;
  u32 entry_count = 3;
  if (slot.node == Node::proc_directory) {
    entries = proc_entries;
    entry_count = 4;
  }
  u32 total = 0;
  while (slot.offset < entry_count) {
    const i32 result = emit_dirent(
        address + total, size - total, slot.offset + 1, slot.offset + 1,
        directory_type, entries[slot.offset]);
    if (result == 0) {
      break;
    }
    total += static_cast<u32>(result);
    ++slot.offset;
  }
  return static_cast<i32>(total);
}

struct [[gnu::packed]] Stat64 {
  u64 device;
  u64 inode;
  u32 mode;
  u32 links;
  u32 uid;
  u32 gid;
  u64 special_device;
  u64 padding1;
  u64 size;
  u32 block_size;
  u32 padding2;
  u64 blocks;
  u64 timestamps[6];
  u32 reserved[2];
};

static_assert(sizeof(Stat64) == 128);

[[nodiscard]] i32 write_stat(Node node, u32 address) {
  if (!user_memory.contains(address, sizeof(Stat64))) {
    return error(Errno::bad_address);
  }
  Stat64 value{};
  constexpr u32 directory_mode = 0040000 | 0755;
  constexpr u32 regular_mode = 0100000 | 0444;
  constexpr u32 character_mode = 0020000 | 0666;
  value.inode = static_cast<u32>(node);
  value.mode = node == Node::none
                   ? character_mode
                   : (directory(node) ? directory_mode : regular_mode);
  value.links = directory(node) ? 2 : 1;
  value.size = node_size(node);
  value.block_size = 4096;
  memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  return 0;
}

[[maybe_unused, nodiscard]] i32 fstat64(u32 descriptor, u32 address) {
  if (descriptor < 3) {
    return write_stat(Node::none, address);
  }
  if (descriptor >= 16) {
    return error(Errno::bad_file_descriptor);
  }
  return write_stat(descriptors[descriptor - 3].node, address);
}

struct [[gnu::packed]] Statx {
  u32 mask;
  u32 block_size;
  u64 attributes;
  u32 links;
  u32 uid;
  u32 gid;
  u16 mode;
  u16 padding;
  u64 inode;
  u64 size;
  u64 blocks;
  u64 attributes_mask;
  u8 remainder[192];
};

static_assert(sizeof(Statx) == 256);

[[maybe_unused, nodiscard]] i32 write_statx(Node node, u32 address) {
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  if (!user_memory.contains(address, sizeof(Statx))) {
    return error(Errno::bad_address);
  }
  constexpr u16 directory_mode = 0040000 | 0755;
  constexpr u16 regular_mode = 0100000 | 0444;
  Statx value{};
  value.mask = 0x7ff;
  value.block_size = 4096;
  value.links = directory(node) ? 2 : 1;
  value.mode = directory(node) ? directory_mode : regular_mode;
  value.inode = static_cast<u32>(node);
  value.size = node_size(node);
  memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  return 0;
}

[[maybe_unused, nodiscard]] i32 readlinkat(u32 path, u32 address, u32 size) {
  if (!user_string_is(path, "/proc/self/exe")) {
    return error(Errno::no_entry);
  }
  constexpr char target[] = "/busybox";
  constexpr u32 target_size = sizeof(target) - 1;
  const u32 count = size < target_size ? size : target_size;
  if (!user_memory.contains(address, count)) {
    return error(Errno::bad_address);
  }
  memcpy(reinterpret_cast<void*>(address), target, count);
  return static_cast<i32>(count);
}

[[maybe_unused, nodiscard]] i32 ppoll64(u32 address, u32 count,
                                       u32 timeout_address) {
  if (count > 16 ||
      !user_memory.contains(address, count * sizeof(Pollfd32))) {
    return error(Errno::bad_address);
  }
  u64 deadline = ~u64{0};
  if (timeout_address != 0) {
    if (!user_memory.contains(timeout_address, sizeof(Timespec64))) {
      return error(Errno::bad_address);
    }
    const auto timeout =
        *reinterpret_cast<const Timespec64*>(timeout_address);
    constexpr u64 ticks_per_second = 10'000'000;
    const u64 duration = timeout.seconds * ticks_per_second +
                         static_cast<u32>(timeout.nanoseconds) / 100;
    deadline = arch::time_ticks() + duration;
  }
  auto* descriptors = reinterpret_cast<Pollfd32*>(address);
  for (;;) {
    i32 ready_count = 0;
    for (u32 i = 0; i < count; ++i) {
      descriptors[i].returned_events = 0;
      constexpr u16 poll_input = 1;
      if (descriptors[i].descriptor == 0 &&
          (descriptors[i].events & poll_input) != 0 &&
          drivers::uart::ready()) {
        descriptors[i].returned_events = poll_input;
        ++ready_count;
      }
    }
    if (ready_count != 0 || arch::time_ticks() >= deadline) {
      return ready_count;
    }
  }
}

[[maybe_unused, nodiscard]] i32 chdir(u32 path) {
  const Node node = path_node(path);
  if (node == Node::root_directory) {
    process.cwd_proc = false;
    return 0;
  }
  if (node == Node::proc_directory) {
    process.cwd_proc = true;
    return 0;
  }
  return node == Node::none ? error(Errno::no_entry)
                            : error(Errno::invalid_argument);
}

[[nodiscard]] i32 execve(TrapFrame& frame, u32 path, u32 argv) {
  if (!parent.active || !user_string_is(path, "/proc/self/exe") ||
      !user_memory.aligned(argv, alignof(u32))) {
    return error(Errno::no_entry);
  }
  constexpr u32 max_arguments = 16;
  constexpr u32 max_argument_size = 128;
  char argument_storage[max_arguments][max_argument_size]{};
  const char* arguments[max_arguments]{};
  u32 count = 0;
  for (; count < max_arguments; ++count) {
    const u32 slot = argv + count * sizeof(u32);
    if (!user_memory.contains(slot, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    const u32 address = *reinterpret_cast<const u32*>(slot);
    if (address == 0) {
      break;
    }
    if (!copy_user_string(address, argument_storage[count],
                          max_argument_size)) {
      return error(Errno::bad_address);
    }
    arguments[count] = argument_storage[count];
  }
  if (count == 0 || count == max_arguments || !save_parent_memory() ||
      !replace_with_busybox(frame, arguments, count)) {
    return error(Errno::no_memory);
  }
  return 0;
}

[[nodiscard]] i32 wait4(u32 pid, u32 status) {
  if (!parent.child_waitable || (pid != ~u32{0} && pid != 2)) {
    return error(Errno::invalid_argument);
  }
  if (status != 0) {
    if (!user_memory.contains(status, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u32*>(status) = parent.child_status << 8;
  }
  parent.child_waitable = false;
  return 2;
}

struct [[gnu::packed]] ChildSiginfo {
  i32 signal;
  i32 error_number;
  i32 code;
  i32 pid;
  u32 uid;
  i32 status;
  u32 user_ticks;
  u32 system_ticks;
  u8 remainder[96];
};

static_assert(sizeof(ChildSiginfo) == 128);

[[nodiscard]] i32 waitid(u32 which, u32 pid, u32 address) {
  constexpr u32 all_children = 0;
  constexpr u32 specific_pid = 1;
  if ((which != all_children && which != specific_pid) ||
      (which == specific_pid && pid != 2) || !parent.child_waitable) {
    return error(Errno::invalid_argument);
  }
  if (!user_memory.contains(address, sizeof(ChildSiginfo))) {
    return error(Errno::bad_address);
  }
  constexpr i32 sigchld = 17;
  constexpr i32 child_exited = 1;
  const ChildSiginfo information{sigchld,
                                 0,
                                 child_exited,
                                 2,
                                 0,
                                 static_cast<i32>(parent.child_status),
                                 0,
                                 0,
                                 {}};
  memcpy(reinterpret_cast<void*>(address), &information,
         sizeof(information));
  parent.child_waitable = false;
  return 0;
}

#ifdef MIKOS_TRIBE_INTERACTIVE
struct [[gnu::packed]] Termios32 {
  u32 input_flags;
  u32 output_flags;
  u32 control_flags;
  u32 local_flags;
  u8 line;
  u8 control_character[19];
};

struct [[gnu::packed]] Winsize {
  u16 rows;
  u16 columns;
  u16 horizontal_pixels;
  u16 vertical_pixels;
};
#endif

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
  if (descriptor >= 3) {
    return read_virtual(descriptor, address, size);
  }
  if (descriptor != 0) {
    return error(Errno::bad_file_descriptor);
  }
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  if (size == 0) {
    return 0;
  }
  u8 data{};
  while (!drivers::uart::receive(data)) {
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  auto echo_input = [](u8 value) {
    if (value == 0x7f || value == 0x08) {
      uart_put('\b');
      uart_put(' ');
      uart_put('\b');
    } else {
      uart_put(static_cast<char>(value));
    }
  };
  echo_input(data);
#endif
  auto* output = reinterpret_cast<u8*>(address);
  output[0] = data;
  u32 count = 1;
  while (count < size && drivers::uart::receive(data)) {
#ifdef MIKOS_TRIBE_INTERACTIVE
    echo_input(data);
#endif
    output[count++] = data;
  }
  return static_cast<i32>(count);
}

[[nodiscard]] i32 getcwd(u32 address, u32 size) {
  const u32 required = process.cwd_proc ? 6 : 2;
  if (size < required || !user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  auto* output = reinterpret_cast<char*>(address);
  constexpr char root[] = "/";
  constexpr char proc[] = "/proc";
  memcpy(output, process.cwd_proc ? proc : root, required);
  return static_cast<i32>(required);
}

#ifdef MIKOS_TRIBE_INTERACTIVE
[[nodiscard]] i32 uart_ioctl(u32 descriptor, u32 request, u32 address) {
  if (descriptor > 2) {
    return error(Errno::bad_file_descriptor);
  }
  constexpr u32 tcgets = 0x5401;
  constexpr u32 tcsets = 0x5402;
  constexpr u32 tcsetsw = 0x5403;
  constexpr u32 tcsetsf = 0x5404;
  constexpr u32 tiocgwinsz = 0x5413;
  if (request == tcgets) {
    if (!user_memory.contains(address, sizeof(Termios32))) {
      return error(Errno::bad_address);
    }
    constexpr Termios32 terminal{
        0x00000500,  // ICRNL | IXON
        0x00000005,  // OPOST | ONLCR
        0x000000bf,  // B38400 | CS8 | CREAD
        0x0000803b,  // ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN
        0,
        {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0,
         0}};
    memcpy(reinterpret_cast<void*>(address), &terminal, sizeof(terminal));
    return 0;
  }
  if (request == tcsets || request == tcsetsw || request == tcsetsf) {
    return user_memory.contains(address, sizeof(Termios32))
               ? 0
               : error(Errno::bad_address);
  }
  if (request == tiocgwinsz) {
    if (!user_memory.contains(address, sizeof(Winsize))) {
      return error(Errno::bad_address);
    }
    constexpr Winsize window{24, 80, 0, 0};
    memcpy(reinterpret_cast<void*>(address), &window, sizeof(window));
    return 0;
  }
  return error(Errno::not_a_tty);
}
#endif

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
#ifndef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:ENOSYS ");
  write_u32(number);
  write_text(" ");
  write_text(abi::riscv32::name(number));
  write_text("\n");
#else
  static_cast<void>(number);
#endif
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
#ifdef MIKOS_TRACE_SYSCALLS
  write_text("MIKOS:SYSCALL ");
  write_u32(number);
  write_text("\n");
#endif
  const u32 a0 = frame.x[10];
  const u32 a1 = frame.x[11];
  const u32 a2 = frame.x[12];
  const u32 a3 = frame.x[13];
#ifdef MIKOS_TRIBE_INTERACTIVE
  const u32 a4 = frame.x[14];
#endif
  switch (static_cast<Syscall>(number)) {
    case Syscall::getcwd:
      return getcwd(a0, a1);
    case Syscall::write:
      return write(a0, a1, a2);
    case Syscall::writev:
      return writev(a0, a1, a2);
    case Syscall::read:
      return read(a0, a1, a2);
    case Syscall::chdir:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return chdir(a0);
#else
      return unknown(number);
#endif
    case Syscall::faccessat:
      return user_string_is(a1, ".") ? 0 : error(Errno::no_entry);
    case Syscall::openat:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return openat(a1);
#else
      return error(Errno::no_entry);
#endif
    case Syscall::close:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return close(a0);
#else
      return unknown(number);
#endif
    case Syscall::getdents64:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return getdents64(a0, a1, a2);
#else
      return unknown(number);
#endif
    case Syscall::lseek:
#ifdef MIKOS_TRIBE_INTERACTIVE
      if (a0 < 3 || a0 >= 16 ||
          descriptors[a0 - 3].node == Node::none) {
        return error(Errno::bad_file_descriptor);
      }
      descriptors[a0 - 3].offset = a1;
      return static_cast<i32>(a1);
#else
      return unknown(number);
#endif
    case Syscall::exit:
    case Syscall::exit_group:
      if (parent.active) {
        restore_parent(frame, a0);
        return 2;
      }
      if (process.image == 0) {
        write_text("MIKOS:BUSYBOX_EXIT ");
        write_u32(a0);
        write_text("\n");
        if (a0 != 0) {
          shutdown(a0);
        }
#ifdef MIKOS_TRIBE
        write_text("MIKOS:EXIT 0\n");
        shutdown(0);
#else
        start_stress_ng(frame);
        return 0;
#endif
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
    case Syscall::waitid:
      return waitid(a0, a1, a2);
    case Syscall::set_tid_address:
      return 1;
    case Syscall::set_robust_list:
      return 0;
    case Syscall::rseq:
      return error(Errno::no_syscall);
    case Syscall::getpid:
    case Syscall::gettid:
      return static_cast<i32>(process.pid);
    case Syscall::getppid:
      return process.pid == 1 ? 0 : 1;
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
    case Syscall::clone:
      return clone(frame, a0, a1);
    case Syscall::execve:
      return execve(frame, a0, a1);
    case Syscall::wait4:
      return wait4(a0, a1);
    case Syscall::mmap2:
      return mmap(a0, a1, a3);
    case Syscall::munmap:
    case Syscall::mprotect:
      return protect_or_unmap(a0, a1);
    case Syscall::riscv_hwprobe:
      return error(Errno::no_syscall);
    case Syscall::statx:
#ifdef MIKOS_TRIBE_INTERACTIVE
      if (user_string_is(a1, "") && a0 >= 3 && a0 < 16) {
        return write_statx(descriptors[a0 - 3].node, a4);
      }
      return write_statx(path_node(a1), a4);
#else
      return unknown(number);
#endif
    case Syscall::getrandom:
      return getrandom(a0, a1);
    case Syscall::clock_gettime32:
      return clock_gettime(a1, false);
    case Syscall::clock_gettime64:
      return clock_gettime(a1, true);
    case Syscall::clock_nanosleep64:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return 0;
#else
      return unknown(number);
#endif
    case Syscall::ppoll64:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return ppoll64(a0, a1, a2);
#else
      return unknown(number);
#endif
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
    case Syscall::prctl:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return 0;
#else
      return unknown(number);
#endif
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
#ifdef MIKOS_TRIBE_INTERACTIVE
      return uart_ioctl(a0, a1, a2);
#else
      return error(Errno::not_a_tty);
#endif
    case Syscall::rt_sigaction:
    case Syscall::rt_sigprocmask:
    case Syscall::sigaltstack:
      return 0;
    case Syscall::statfs64:
      return unknown(number);
    case Syscall::readlinkat:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return readlinkat(a1, a2, a3);
#else
      return unknown(number);
#endif
    case Syscall::fstatat64: {
#ifdef MIKOS_TRIBE_INTERACTIVE
      const Node node = user_string_is(a1, "") && a0 >= 3 && a0 < 16
                            ? descriptors[a0 - 3].node
                            : path_node(a1);
      return node == Node::none ? error(Errno::no_entry)
                                : write_stat(node, a2);
#else
      return unknown(number);
#endif
    }
    case Syscall::fstat64:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return fstat64(a0, a1);
#else
      return unknown(number);
#endif
  }
  return unknown(number);
}

}  // namespace mikos
