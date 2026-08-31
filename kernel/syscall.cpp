#include <mikos/arch.hpp>
#include <drivers/fs/root.hpp>
#include <kernel/pseudo_filesystem.hpp>
#include <drivers/uart/uart.hpp>
#include <kernel/path.hpp>
#include <mikos/kernel.hpp>
#include <mikos/abi/riscv32.hpp>
#include <mikos/abi/socket.hpp>
#include <mikos/process/model.hpp>
#include <mikos/process/pipe.hpp>
#include <mikos/process/signal.hpp>
#include <mikos/process/snapshot_arena.hpp>
#include <mikos/process/pty.hpp>

extern "C" void* memset(void*, int, mikos::usize);
extern "C" void* memcpy(void*, const void*, mikos::usize);
extern "C" unsigned char __fork_snapshot_arena_begin[];
extern "C" unsigned char __fork_snapshot_arena_end[];

namespace mikos {
namespace {

using abi::riscv32::Errno;
using abi::riscv32::Syscall;
using abi::riscv32::error;
using PseudoFilesystem = pseudo_fs::Filesystem;

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

struct ActiveSignalFrame {
  TrapFrame frame{};
  u64 blocked{};
  bool active{};
};

struct SuspendedParent {
  TrapFrame frame{};
  u32 brk{};
  u32 mmap_begin{};
  u32 mmap_cursor{};
  u32 mutable_begin{};
  u32 stack_begin{};
  u32 mutable_size{};
  u32 mmap_size{};
  u32 stack_size{};
  u32 child_status{};
  u32 child_pid{};
  u32 child_process_group{};
  process_model::ProcessLineage lineage{};
  u32 child_tid_address{};
  u32 process_group{};
  u32 session{};
  u32 image{};
  process_model::SignalState signals{};
  ActiveSignalFrame signal_frame{};
  u32 file_creation_mask{};
  char current_directory[256]{};
  char executable_path[256]{};
  process_model::SnapshotAllocation snapshot{};
  bool active{};
  bool memory_saved{};
  bool child_waitable{};
};

SuspendedParent parent{};
// Process metadata remains bounded independently of address-space size.
// Snapshots consume their exact byte count from the shared arena rather than
// reserving this capacity times a worst-case image size.
inline constexpr u32 process_context_capacity = 32;
inline constexpr u32 nested_ancestor_capacity = process_context_capacity;
SuspendedParent nested_ancestors[nested_ancestor_capacity]{};
u32 nested_ancestor_depth{};
struct WaitableChild {
  u32 pid{};
  u32 parent_pid{};
  u32 process_group{};
  u32 status{};
  bool used{};
};

WaitableChild waitable_children[process_context_capacity]{};
u32 file_creation_mask{0022};
u32 child_tid_address{};
process_model::SignalState signals{};
ActiveSignalFrame active_signal_frame{};
// At most one free extent can be introduced per live context release. Keep
// fragmentation metadata above the process-context bound plus the parked
// background and PTY contexts.
process_model::SnapshotArena<64> snapshot_arena{};
bool snapshot_arena_initialized{};

enum class BackgroundWait : u8 { none, socket_read };

struct ParkedBackground {
  TrapFrame frame{};
  u32 brk{};
  u32 mmap_begin{};
  u32 mmap_cursor{};
  u32 mutable_begin{};
  u32 stack_begin{};
  u32 mutable_size{};
  u32 mmap_size{};
  u32 stack_size{};
  u32 pid{};
  u32 parent_pid{};
  u32 process_group{};
  u32 session{};
  u32 image{};
  u32 child_tid_address{};
  process_model::SignalState signals{};
  ActiveSignalFrame signal_frame{};
  u32 file_creation_mask{};
  char current_directory[256]{};
  char executable_path[256]{};
  process_model::SnapshotAllocation snapshot{};
  u8 wait_socket{network::invalid_socket};
  BackgroundWait wait{BackgroundWait::none};
  bool used{};
};

ParkedBackground background{};

// The flat address-space adapter normally runs a fork child to completion.
// An interactive PTY shell is different: after writing its prompt it must
// block on the slave while its immediate Dropbear parent drains the master,
// forwards the prompt, and receives more input. Its exact-sized snapshot uses
// the same shared arena as every other suspended address space.
struct ParkedInteractiveChild {
  TrapFrame frame{};
  u32 brk{};
  u32 mmap_begin{};
  u32 mmap_cursor{};
  u32 mutable_begin{};
  u32 stack_begin{};
  u32 mutable_size{};
  u32 mmap_size{};
  u32 stack_size{};
  u32 pid{};
  u32 parent_pid{};
  u32 process_group{};
  u32 session{};
  u32 image{};
  u32 child_tid_address{};
  process_model::SignalState signals{};
  ActiveSignalFrame signal_frame{};
  u32 file_creation_mask{};
  char current_directory[256]{};
  char executable_path[256]{};
  process_model::PtyHandle wait_pty{};
  process_model::PtyEnd wait_end{process_model::PtyEnd::slave};
  process_model::SnapshotAllocation snapshot{};
  bool used{};
};

ParkedInteractiveChild interactive_child{};
// When a foreground program below the shell blocks on the PTY, its immediate
// parent is the waiting shell rather than Dropbear. Preserve that parent fork
// frame while the nearest PTY-master owner services the connection.
SuspendedParent interactive_parent{};
u32 interactive_relay_service_pid{};

[[maybe_unused, nodiscard]] bool user_string_is(u32 address,
                                                const char* expected);
void reset_descriptors();
[[nodiscard]] bool save_parent_descriptors();
void restore_parent_descriptors();
void discard_parent_descriptors();
[[nodiscard]] bool park_background(TrapFrame& frame, u8 wait_socket);
[[nodiscard]] bool resume_background_if_ready(TrapFrame& frame);
[[nodiscard]] bool park_interactive_child(
    TrapFrame& frame, process_model::PtyHandle wait_pty,
    process_model::PtyEnd wait_end);
[[nodiscard]] bool resume_interactive_child_if_ready(TrapFrame& frame);
[[nodiscard]] bool stack_suspended_ancestor();
void reinstate_suspended_ancestor(TrapFrame& frame);

[[nodiscard]] bool waitable_space() {
  for (const auto& child : waitable_children) {
    if (!child.used) {
      return true;
    }
  }
  return false;
}

void publish_child_exit(u32 pid, u32 parent_pid, u32 process_group,
                        u32 status) {
  for (auto& child : waitable_children) {
    if (!child.used) {
      child = {pid, parent_pid, process_group, status, true};
      parent.child_waitable = true;
      return;
    }
  }
}

void refresh_child_waitable() {
  parent.child_waitable = false;
  for (const auto& child : waitable_children) {
    parent.child_waitable = parent.child_waitable || child.used;
  }
}

[[nodiscard]] bool ensure_snapshot_arena() {
  if (snapshot_arena_initialized) {
    return true;
  }
  const auto begin = static_cast<u32>(
      reinterpret_cast<usize>(__fork_snapshot_arena_begin));
  const auto end = static_cast<u32>(
      reinterpret_cast<usize>(__fork_snapshot_arena_end));
  snapshot_arena_initialized =
      snapshot_arena.initialize(begin, end) ==
      process_model::SnapshotArenaStatus::success;
#ifdef MIKOS_TRIBE_INTERACTIVE
  if (snapshot_arena_initialized) {
    write_text("MIKOS:FORK_ARENA begin=");
    write_u32(begin);
    write_text(" end=");
    write_u32(end);
    write_text(" bytes=");
    write_u32(snapshot_arena.capacity());
    write_text("\n");
  }
#endif
  return snapshot_arena_initialized;
}

[[nodiscard]] bool allocate_snapshot(
    u32 size, process_model::SnapshotAllocation& allocation) {
  if (!ensure_snapshot_arena()) {
    return false;
  }
  allocation = snapshot_arena.allocate(size);
  return allocation.valid();
}

void release_snapshot(process_model::SnapshotAllocation& allocation) {
  if (!allocation.valid()) {
    return;
  }
  if (snapshot_arena.release(allocation) !=
      process_model::SnapshotArenaStatus::success) {
    write_text("MIKOS:FORK_ARENA_CORRUPT\n");
    shutdown(9);
  }
  allocation = {};
}

[[nodiscard]] bool save_parent_memory() {
  if (parent.memory_saved) {
    return true;
  }
  if (parent.mutable_begin > parent.brk ||
      parent.mmap_cursor < parent.mmap_begin ||
      parent.stack_begin > user_stack_top) {
    return false;
  }
  parent.mutable_size = parent.brk - parent.mutable_begin;
  parent.mmap_size = parent.mmap_cursor - parent.mmap_begin;
  parent.stack_size = user_stack_top - parent.stack_begin;
  const u64 total = static_cast<u64>(parent.mutable_size) +
                    parent.mmap_size + parent.stack_size;
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:CLONE_COPY mutable=");
  write_u32(parent.mutable_size);
  write_text(" mmap=");
  write_u32(parent.mmap_size);
  write_text(" stack=");
  write_u32(parent.stack_size);
  write_text("\n");
#endif
  if (total > ~u32{0} ||
      !allocate_snapshot(static_cast<u32>(total), parent.snapshot)) {
#ifdef MIKOS_TRIBE_INTERACTIVE
    write_text("MIKOS:FORK_ARENA_ENOMEM requested=");
    write_u32(total > ~u32{0} ? ~u32{0} : static_cast<u32>(total));
    write_text(" available=");
    write_u32(snapshot_arena.available());
    write_text(" largest=");
    write_u32(snapshot_arena.largest_available());
    write_text("\n");
#endif
    return false;
  }
  auto* snapshot = reinterpret_cast<u8*>(parent.snapshot.address);
  u32 cursor = 0;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(parent.mutable_begin),
         parent.mutable_size);
  cursor += parent.mutable_size;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(parent.mmap_begin),
         parent.mmap_size);
  cursor += parent.mmap_size;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(parent.stack_begin),
         parent.stack_size);
  parent.memory_saved = true;
  return true;
}

void resume_parent(TrapFrame& frame, u32 status, bool child_completed,
                   bool return_from_clone) {
  const u32 child_process_group = process.process_group;
  // Flat-address-space payloads overlap. Reload the suspended executable's
  // segments, then apply its private writable/mmap/stack snapshot.
  if (process.image != parent.image &&
      !restore_executable_image(parent.executable_path)) {
    write_text("MIKOS:PROCESS_IMAGE_RESTORE_FAIL\n");
    shutdown(9);
  }
  if (parent.memory_saved) {
    const auto* snapshot =
        reinterpret_cast<const u8*>(parent.snapshot.address);
    u32 cursor = 0;
    memcpy(reinterpret_cast<void*>(parent.mutable_begin),
           snapshot + cursor, parent.mutable_size);
    cursor += parent.mutable_size;
    memcpy(reinterpret_cast<void*>(parent.mmap_begin), snapshot + cursor,
           parent.mmap_size);
    cursor += parent.mmap_size;
    memcpy(reinterpret_cast<void*>(parent.stack_begin), snapshot + cursor,
           parent.stack_size);
    release_snapshot(parent.snapshot);
  }
  frame = parent.frame;
  if (return_from_clone) {
    frame.x[10] = parent.child_pid;
    frame.mepc += 4;
  }
  process.brk = parent.brk;
  process.mmap_begin = parent.mmap_begin;
  process.mmap_cursor = parent.mmap_cursor;
  process.mutable_begin = parent.mutable_begin;
  process.pid = parent.lineage.pid;
  process.parent_pid = parent.lineage.parent_pid;
  child_tid_address = parent.child_tid_address;
  process.process_group = parent.process_group;
  process.session = parent.session;
  signals = parent.signals;
  active_signal_frame = parent.signal_frame;
  process.image = parent.image;
  file_creation_mask = parent.file_creation_mask;
  memcpy(process.current_directory, parent.current_directory,
         sizeof(process.current_directory));
  memcpy(process.executable_path, parent.executable_path,
         sizeof(process.executable_path));
  process.image_replaced = true;
  if (child_completed) {
    parent.child_status = status;
    parent.child_process_group = child_process_group;
    publish_child_exit(parent.child_pid, parent.lineage.pid,
                       child_process_group, status);
    // Linux publishes the zombie before making SIGCHLD observable.  Queue the
    // signal in the just-restored immediate parent; nested flat-address-space
    // suspension may subsequently put an older ancestor underneath it.
    static_cast<void>(signals.queue(process_model::signal_child));
  }
  parent.active = false;
  parent.memory_saved = false;
  restore_parent_descriptors();
}

void restore_parent(TrapFrame& frame, u32 status) {
  resume_parent(frame, status, true, true);
}

[[nodiscard]] i32 clone(TrapFrame& frame, u32 flags, u32 stack,
                        u32 child_tid) {
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:CLONE_START\n");
#endif
  constexpr u32 signal_mask = 0xff;
  constexpr u32 sigchld = 17;
  constexpr u32 clone_vm = 0x100;
  constexpr u32 clone_vfork = 0x4000;
  constexpr u32 clone_child_cleartid = 0x00200000;
  constexpr u32 clone_child_settid = 0x01000000;
  const u32 behavior = flags & ~signal_mask;
  constexpr u32 fork_bookkeeping =
      clone_child_cleartid | clone_child_settid;
  const bool ordinary_fork = (behavior & ~fork_bookkeeping) == 0;
  const bool vfork = behavior == (clone_vm | clone_vfork);
  const u32 child_stack = stack == 0 ? frame.x[2] : stack;
  const bool nested_background_fork =
      parent.active && process.pid == parent.child_pid;
  if ((!ordinary_fork && !vfork) || (flags & signal_mask) != sigchld ||
      !user_memory.contains(child_stack, 1) ||
      (((flags & (clone_child_settid | clone_child_cleartid)) != 0) &&
       (!user_memory.contains(child_tid, sizeof(u32)) ||
        !user_memory.aligned(child_tid, alignof(u32))))) {
    return error(Errno::invalid_argument);
  }
  if (parent.active && !nested_background_fork) {
    return error(Errno::invalid_argument);
  }
  if ((nested_background_fork &&
       nested_ancestor_depth == nested_ancestor_capacity) ||
      !waitable_space()) {
    return error(Errno::no_memory);
  }
  if (nested_background_fork && !stack_suspended_ancestor()) {
    return error(Errno::no_memory);
  }
  if (!save_parent_descriptors()) {
    reinstate_suspended_ancestor(frame);
    return error(Errno::no_memory);
  }
  parent.frame = frame;
  parent.brk = process.brk;
  parent.mmap_begin = process.mmap_begin;
  parent.mmap_cursor = process.mmap_cursor;
  parent.mutable_begin = process.mutable_begin;
  parent.file_creation_mask = file_creation_mask;
  memcpy(parent.current_directory, process.current_directory,
         sizeof(parent.current_directory));
  memcpy(parent.executable_path, process.executable_path,
         sizeof(parent.executable_path));
  parent.stack_begin = align_down(child_stack, static_cast<u32>(16));
  parent.lineage = {process.pid, process.parent_pid};
  parent.child_tid_address = child_tid_address;
  parent.process_group = process.process_group;
  parent.session = process.session;
  parent.image = process.image;
  parent.signals = signals;
  parent.signal_frame = active_signal_frame;
  static u32 next_pid = 2;
  parent.child_pid = next_pid++;
  parent.active = true;
  parent.memory_saved = false;
  if (!save_parent_memory()) {
    parent.active = false;
    discard_parent_descriptors();
    reinstate_suspended_ancestor(frame);
    return error(Errno::no_memory);
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:CLONE_DONE\n");
#endif
  const auto child_lineage = process_model::fork_lineage(
      parent.lineage, parent.child_pid);
  process.pid = child_lineage.pid;
  process.parent_pid = child_lineage.parent_pid;
  child_tid_address =
      (flags & clone_child_cleartid) != 0 ? child_tid : 0;
  if ((flags & clone_child_settid) != 0) {
    *reinterpret_cast<u32*>(child_tid) = process.pid;
  }
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

struct Node {
  enum class Kind : u8 {
    none,
    standard_input,
    standard_output,
    filesystem,
    pseudo,
    network_socket,
    pipe,
    pty,
  };

  static constexpr Kind none = Kind::none;
  static constexpr Kind standard_input = Kind::standard_input;
  static constexpr Kind standard_output = Kind::standard_output;
  static constexpr Kind filesystem = Kind::filesystem;
  static constexpr Kind pseudo = Kind::pseudo;
  static constexpr Kind network_socket = Kind::network_socket;
  static constexpr Kind pipe = Kind::pipe;
  static constexpr Kind pty = Kind::pty;

  Kind kind{Kind::none};
  drivers::fs::root::Node file{};
  PseudoFilesystem::Node pseudo_node{PseudoFilesystem::Node::none};
  u8 pty_number{0xff};

  constexpr Node() = default;
  constexpr Node(Kind value) : kind(value) {}
  constexpr Node(Kind value, u8 number) : kind(value), pty_number(number) {}
  constexpr Node(const drivers::fs::root::Node& value)
      : kind(Kind::filesystem), file(value) {}
  constexpr Node(PseudoFilesystem::Node value)
      : kind(Kind::pseudo), pseudo_node(value) {}
  [[nodiscard]] constexpr operator Kind() const { return kind; }
};

struct Descriptor {
  Node node{};
  u32 offset{};
  u32 flags{};
  u8 socket{network::invalid_socket};
  process_model::PipeHandle pipe{};
  process_model::PipeEnd pipe_end{process_model::PipeEnd::read};
  process_model::PtyHandle pty{};
  process_model::PtyEnd pty_end{process_model::PtyEnd::master};
  char path[path::capacity]{};
};

Descriptor descriptors[13]{};
Descriptor standard_redirects[3]{};
Descriptor parent_descriptors[13]{};
Descriptor parent_standard_redirects[3]{};
Descriptor background_descriptors[13]{};
Descriptor background_standard_redirects[3]{};
Descriptor nested_ancestor_descriptors[nested_ancestor_capacity][13]{};
Descriptor nested_ancestor_standard_redirects[nested_ancestor_capacity][3]{};
Descriptor interactive_child_descriptors[13]{};
Descriptor interactive_child_standard_redirects[3]{};
Descriptor interactive_parent_descriptors[13]{};
Descriptor interactive_parent_standard_redirects[3]{};
// A Dropbear command session holds its signal pipe and listener child-status
// pipe while creating stdin, stdout, and stderr pipes for the remote command.
// Keep spare bounded slots for cleanup/reaping paths and future PTY helpers.
process_model::PipeTable<8> pipes{};
process_model::PtyTable<4> ptys{};

[[nodiscard]] bool owns_pty_master(
    const Descriptor* saved, const Descriptor* saved_standard,
    process_model::PtyHandle handle) {
  for (u32 i = 0; i < 13; ++i) {
    if (saved[i].node == Node::pty && saved[i].pty == handle &&
        saved[i].pty_end == process_model::PtyEnd::master) {
      return true;
    }
  }
  for (u32 i = 0; i < 3; ++i) {
    if (saved_standard[i].node == Node::pty &&
        saved_standard[i].pty == handle &&
        saved_standard[i].pty_end == process_model::PtyEnd::master) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool parent_owns_pty_master(
    process_model::PtyHandle handle) {
  return owns_pty_master(parent_descriptors,
                         parent_standard_redirects, handle);
}

[[nodiscard]] bool nearest_ancestor_owns_pty_master(
    process_model::PtyHandle handle) {
  if (nested_ancestor_depth == 0) {
    return false;
  }
  const u32 level = nested_ancestor_depth - 1;
  return owns_pty_master(nested_ancestor_descriptors[level],
                         nested_ancestor_standard_redirects[level], handle);
}

[[nodiscard]] u32 queue_process_group_signal(u32 process_group, u8 signal) {
  if (process_group == 0 || !process_model::SignalState::valid(signal)) {
    return 0;
  }
  u32 delivered_pids[nested_ancestor_capacity + 4]{};
  u32 delivered = 0;
  const auto queue = [&](u32 pid, u32 group,
                         process_model::SignalState& state) {
    if (pid == 0 || group != process_group) {
      return;
    }
    for (u32 i = 0; i < delivered; ++i) {
      if (delivered_pids[i] == pid) {
        return;
      }
    }
    if (state.queue(signal) == process_model::SignalStatus::success) {
      delivered_pids[delivered++] = pid;
    }
  };

  queue(process.pid, process.process_group, signals);
  if (parent.active) {
    queue(parent.lineage.pid, parent.process_group, parent.signals);
  }
  for (auto& ancestor : nested_ancestors) {
    if (ancestor.active) {
      queue(ancestor.lineage.pid, ancestor.process_group, ancestor.signals);
    }
  }
  // A shell that has run an external command can remain represented in the
  // background handoff metadata after it subsequently parks on its PTY. The
  // interactive snapshot is then authoritative; visit it first so PID
  // de-duplication cannot queue a terminal signal into the stale copy.
  if (interactive_child.used) {
    queue(interactive_child.pid, interactive_child.process_group,
          interactive_child.signals);
  }
  if (background.used) {
    queue(background.pid, background.process_group, background.signals);
  }
  return delivered;
}

[[nodiscard]] bool retain_descriptor(const Descriptor& descriptor) {
  if (descriptor.node == Node::network_socket) {
    return network::socket_retain(descriptor.socket) ==
           network::SocketResult::success;
  }
  if (descriptor.node == Node::pipe) {
    return pipes.retain(descriptor.pipe, descriptor.pipe_end) ==
           process_model::PipeStatus::success;
  }
  if (descriptor.node == Node::pty) {
    return ptys.retain(descriptor.pty, descriptor.pty_end) ==
           process_model::PtyStatus::success;
  }
  return true;
}

void release_descriptor(Descriptor& descriptor) {
  if (descriptor.node == Node::network_socket) {
    static_cast<void>(network::socket_close(descriptor.socket));
  } else if (descriptor.node == Node::pipe) {
    static_cast<void>(pipes.release(descriptor.pipe, descriptor.pipe_end));
  } else if (descriptor.node == Node::pty) {
    static_cast<void>(ptys.release(descriptor.pty, descriptor.pty_end));
  }
  descriptor = {};
}

void reset_descriptors() {
  for (auto& descriptor : descriptors) {
    release_descriptor(descriptor);
  }
  for (auto& descriptor : standard_redirects) {
    release_descriptor(descriptor);
  }
}

[[nodiscard]] bool save_parent_descriptors() {
  for (auto& descriptor : parent_descriptors) {
    release_descriptor(descriptor);
  }
  for (auto& descriptor : parent_standard_redirects) {
    release_descriptor(descriptor);
  }
  u32 copied = 0;
  for (; copied < 13; ++copied) {
    if (!retain_descriptor(descriptors[copied])) {
      break;
    }
    parent_descriptors[copied] = descriptors[copied];
  }
  if (copied != 13) {
    for (u32 i = 0; i < copied; ++i) {
      release_descriptor(parent_descriptors[i]);
    }
    return false;
  }
  copied = 0;
  for (; copied < 3; ++copied) {
    if (!retain_descriptor(standard_redirects[copied])) {
      break;
    }
    parent_standard_redirects[copied] = standard_redirects[copied];
  }
  if (copied != 3) {
    for (auto& descriptor : parent_descriptors) {
      release_descriptor(descriptor);
    }
    for (u32 i = 0; i < copied; ++i) {
      release_descriptor(parent_standard_redirects[i]);
    }
    return false;
  }
  return true;
}

void restore_parent_descriptors() {
  reset_descriptors();
  for (u32 i = 0; i < 13; ++i) {
    descriptors[i] = parent_descriptors[i];
    parent_descriptors[i] = {};
  }
  for (u32 i = 0; i < 3; ++i) {
    standard_redirects[i] = parent_standard_redirects[i];
    parent_standard_redirects[i] = {};
  }
}

void discard_parent_descriptors() {
  for (auto& descriptor : parent_descriptors) {
    release_descriptor(descriptor);
  }
  for (auto& descriptor : parent_standard_redirects) {
    release_descriptor(descriptor);
  }
}

void move_active_descriptors(Descriptor* saved, Descriptor* saved_standard) {
  for (u32 i = 0; i < 13; ++i) {
    saved[i] = descriptors[i];
    descriptors[i] = {};
  }
  for (u32 i = 0; i < 3; ++i) {
    saved_standard[i] = standard_redirects[i];
    standard_redirects[i] = {};
  }
}

void move_saved_descriptors(Descriptor* saved, Descriptor* saved_standard) {
  reset_descriptors();
  for (u32 i = 0; i < 13; ++i) {
    descriptors[i] = saved[i];
    saved[i] = {};
  }
  for (u32 i = 0; i < 3; ++i) {
    standard_redirects[i] = saved_standard[i];
    saved_standard[i] = {};
  }
}

[[nodiscard]] bool save_interactive_child_memory() {
  interactive_child.mutable_size =
      interactive_child.brk - interactive_child.mutable_begin;
  interactive_child.mmap_size =
      interactive_child.mmap_cursor - interactive_child.mmap_begin;
  interactive_child.stack_size =
      user_stack_top - interactive_child.stack_begin;
  const u64 total = static_cast<u64>(interactive_child.mutable_size) +
                    interactive_child.mmap_size +
                    interactive_child.stack_size;
  if (interactive_child.mutable_begin > interactive_child.brk ||
      interactive_child.mmap_cursor < interactive_child.mmap_begin ||
      interactive_child.stack_begin > user_stack_top ||
      total > ~u32{0} ||
      !allocate_snapshot(static_cast<u32>(total),
                         interactive_child.snapshot)) {
    return false;
  }
  auto* snapshot = reinterpret_cast<u8*>(interactive_child.snapshot.address);
  u32 cursor = 0;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(interactive_child.mutable_begin),
         interactive_child.mutable_size);
  cursor += interactive_child.mutable_size;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(interactive_child.mmap_begin),
         interactive_child.mmap_size);
  cursor += interactive_child.mmap_size;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(interactive_child.stack_begin),
         interactive_child.stack_size);
  return true;
}

void capture_interactive_child(TrapFrame& frame,
                               process_model::PtyHandle wait_pty,
                               process_model::PtyEnd wait_end) {
  interactive_child.frame = frame;
  interactive_child.brk = process.brk;
  interactive_child.mmap_begin = process.mmap_begin;
  interactive_child.mmap_cursor = process.mmap_cursor;
  interactive_child.mutable_begin = process.mutable_begin;
  interactive_child.stack_begin =
      align_down(frame.x[2], static_cast<u32>(16));
  interactive_child.pid = process.pid;
  interactive_child.parent_pid = process.parent_pid;
  interactive_child.process_group = process.process_group;
  interactive_child.session = process.session;
  interactive_child.image = process.image;
  interactive_child.child_tid_address = child_tid_address;
  interactive_child.signals = signals;
  interactive_child.signal_frame = active_signal_frame;
  interactive_child.file_creation_mask = file_creation_mask;
  memcpy(interactive_child.current_directory, process.current_directory,
         sizeof(interactive_child.current_directory));
  memcpy(interactive_child.executable_path, process.executable_path,
         sizeof(interactive_child.executable_path));
  interactive_child.wait_pty = wait_pty;
  interactive_child.wait_end = wait_end;
  interactive_child.used = true;
}

[[nodiscard]] bool park_interactive_child(
    TrapFrame& frame, process_model::PtyHandle wait_pty,
    process_model::PtyEnd wait_end) {
  if (!process_model::pty_child_can_park(
          interactive_child.used, parent.active,
          wait_end == process_model::PtyEnd::slave)) {
    return false;
  }
  const bool direct_parent_services_pty = parent_owns_pty_master(wait_pty);
  const bool relay_through_ancestor =
      !direct_parent_services_pty && !interactive_parent.active &&
      nearest_ancestor_owns_pty_master(wait_pty);
  if (!direct_parent_services_pty && !relay_through_ancestor) {
    return false;
  }
  capture_interactive_child(frame, wait_pty, wait_end);
  if (!save_interactive_child_memory()) {
#ifdef MIKOS_TRIBE_INTERACTIVE
    write_text("MIKOS:PTY_CHILD_PARK_ENOMEM mutable=");
    write_u32(interactive_child.mutable_size);
    write_text(" mmap=");
    write_u32(interactive_child.mmap_size);
    write_text(" stack=");
    write_u32(interactive_child.stack_size);
    write_text("\n");
#endif
    interactive_child = {};
    return false;
  }
  move_active_descriptors(interactive_child_descriptors,
                          interactive_child_standard_redirects);
  if (relay_through_ancestor) {
    // Keep the waiting shell as the blocked program's immediate parent, but
    // temporarily restore the Dropbear ancestor that owns the PTY master.
    // On input, resume_interactive_child_if_ready() stacks Dropbear again,
    // reinstates this fork frame, and only then restores the blocked program.
    interactive_parent = parent;
    for (u32 i = 0; i < 13; ++i) {
      interactive_parent_descriptors[i] = parent_descriptors[i];
      parent_descriptors[i] = {};
    }
    for (u32 i = 0; i < 3; ++i) {
      interactive_parent_standard_redirects[i] =
          parent_standard_redirects[i];
      parent_standard_redirects[i] = {};
    }
    const u32 level = --nested_ancestor_depth;
    parent = nested_ancestors[level];
    nested_ancestors[level] = {};
    for (u32 i = 0; i < 13; ++i) {
      parent_descriptors[i] = nested_ancestor_descriptors[level][i];
      nested_ancestor_descriptors[level][i] = {};
    }
    for (u32 i = 0; i < 3; ++i) {
      parent_standard_redirects[i] =
          nested_ancestor_standard_redirects[level][i];
      nested_ancestor_standard_redirects[level][i] = {};
    }
    interactive_relay_service_pid = parent.lineage.pid;
#ifdef MIKOS_TRIBE_INTERACTIVE
    write_text("MIKOS:PTY_CHILD_RELAY child=");
    write_u32(interactive_child.pid);
    write_text(" parent=");
    write_u32(interactive_parent.lineage.pid);
    write_text(" service=");
    write_u32(interactive_relay_service_pid);
    write_text("\n");
#endif
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:PTY_CHILD_PARK child=");
  write_u32(interactive_child.pid);
  write_text(" parent=");
  write_u32(interactive_child.parent_pid);
  write_text(" bytes=");
  write_u32(interactive_child.mutable_size + interactive_child.mmap_size +
            interactive_child.stack_size);
  write_text("\n");
#endif
  // Return to the immediate Dropbear parent but keep the older suspended
  // listener/shell ancestry stacked. The child will replay this read syscall
  // once the parent has delivered bytes to the PTY master.
  const bool first_park =
      parent.frame.x[17] == static_cast<u32>(Syscall::clone);
  resume_parent(frame, 0, false, first_park);
  return true;
}

[[nodiscard]] bool resume_interactive_child_if_ready(TrapFrame& frame) {
  const bool slave_readable =
      interactive_child.used &&
      ptys.readable(interactive_child.wait_pty, interactive_child.wait_end);
  const bool wait_interrupted =
      interactive_child.used &&
      interactive_child.signals.has_deliverable();
  const bool relayed = interactive_parent.active;
  if (!process_model::pty_child_can_resume(
          interactive_child.used, process.pid, interactive_child.parent_pid,
          parent.active, slave_readable, wait_interrupted,
          relayed ? interactive_relay_service_pid
                  : process_model::invalid_pid) ||
      (relayed && nested_ancestor_depth == nested_ancestor_capacity) ||
      !save_parent_descriptors()) {
    return false;
  }

  parent.frame = frame;
  parent.brk = process.brk;
  parent.mmap_begin = process.mmap_begin;
  parent.mmap_cursor = process.mmap_cursor;
  parent.mutable_begin = process.mutable_begin;
  parent.stack_begin = align_down(frame.x[2], static_cast<u32>(16));
  parent.child_pid =
      relayed ? interactive_parent.lineage.pid : interactive_child.pid;
  parent.lineage = {process.pid, process.parent_pid};
  parent.child_tid_address = child_tid_address;
  parent.process_group = process.process_group;
  parent.session = process.session;
  parent.image = process.image;
  parent.signals = signals;
  parent.signal_frame = active_signal_frame;
  parent.file_creation_mask = file_creation_mask;
  memcpy(parent.current_directory, process.current_directory,
         sizeof(parent.current_directory));
  memcpy(parent.executable_path, process.executable_path,
         sizeof(parent.executable_path));
  parent.active = true;
  parent.memory_saved = false;
  if (!save_parent_memory()) {
    parent.active = false;
    discard_parent_descriptors();
    return false;
  }

  if (relayed) {
    // The active PTY server belongs below the waiting shell in the suspended
    // ancestry. Stack it, then restore the shell as the interactive program's
    // immediate parent so normal exit/wait/SIGCHLD semantics remain intact.
    if (!stack_suspended_ancestor()) {
      write_text("MIKOS:PTY_CHILD_RELAY_STACK_FAIL\n");
      shutdown(9);
    }
    parent = interactive_parent;
    interactive_parent = {};
    for (u32 i = 0; i < 13; ++i) {
      parent_descriptors[i] = interactive_parent_descriptors[i];
      interactive_parent_descriptors[i] = {};
    }
    for (u32 i = 0; i < 3; ++i) {
      parent_standard_redirects[i] =
          interactive_parent_standard_redirects[i];
      interactive_parent_standard_redirects[i] = {};
    }
    interactive_relay_service_pid = 0;
  }

  const u32 child_pid = interactive_child.pid;
  const u32 child_parent_pid = interactive_child.parent_pid;
  reset_descriptors();
  if (process.image != interactive_child.image &&
      !restore_executable_image(interactive_child.executable_path)) {
    write_text("MIKOS:PTY_CHILD_IMAGE_RESTORE_FAIL\n");
    shutdown(9);
  }
  const auto* snapshot =
      reinterpret_cast<const u8*>(interactive_child.snapshot.address);
  u32 cursor = 0;
  memcpy(reinterpret_cast<void*>(interactive_child.mutable_begin),
         snapshot + cursor, interactive_child.mutable_size);
  cursor += interactive_child.mutable_size;
  memcpy(reinterpret_cast<void*>(interactive_child.mmap_begin),
         snapshot + cursor, interactive_child.mmap_size);
  cursor += interactive_child.mmap_size;
  memcpy(reinterpret_cast<void*>(interactive_child.stack_begin),
         snapshot + cursor, interactive_child.stack_size);
  release_snapshot(interactive_child.snapshot);
  move_saved_descriptors(interactive_child_descriptors,
                         interactive_child_standard_redirects);
  frame = interactive_child.frame;
  if (wait_interrupted) {
    frame.x[10] = static_cast<u32>(error(Errno::interrupted));
    frame.mepc += 4;
  }
  process.brk = interactive_child.brk;
  process.mmap_begin = interactive_child.mmap_begin;
  process.mmap_cursor = interactive_child.mmap_cursor;
  process.mutable_begin = interactive_child.mutable_begin;
  process.pid = child_pid;
  process.parent_pid = child_parent_pid;
  process.process_group = interactive_child.process_group;
  process.session = interactive_child.session;
  process.image = interactive_child.image;
  child_tid_address = interactive_child.child_tid_address;
  signals = interactive_child.signals;
  active_signal_frame = interactive_child.signal_frame;
  file_creation_mask = interactive_child.file_creation_mask;
  memcpy(process.current_directory, interactive_child.current_directory,
         sizeof(process.current_directory));
  memcpy(process.executable_path, interactive_child.executable_path,
         sizeof(process.executable_path));
  process.image_replaced = true;
  interactive_child = {};
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:PTY_CHILD_RESUME child=");
  write_u32(child_pid);
  write_text(" parent=");
  write_u32(child_parent_pid);
  write_text("\n");
#endif
  return true;
}

[[nodiscard]] bool save_background_memory() {
  background.mutable_size = background.brk - background.mutable_begin;
  background.mmap_size = background.mmap_cursor - background.mmap_begin;
  background.stack_size = user_stack_top - background.stack_begin;
  const u64 total = static_cast<u64>(background.mutable_size) +
                    background.mmap_size + background.stack_size;
  if (background.mutable_begin > background.brk ||
      background.mmap_cursor < background.mmap_begin ||
      background.stack_begin > user_stack_top ||
      total > ~u32{0} ||
      !allocate_snapshot(static_cast<u32>(total), background.snapshot)) {
    return false;
  }
  auto* snapshot = reinterpret_cast<u8*>(background.snapshot.address);
  u32 cursor = 0;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(background.mutable_begin),
         background.mutable_size);
  cursor += background.mutable_size;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(background.mmap_begin),
         background.mmap_size);
  cursor += background.mmap_size;
  memcpy(snapshot + cursor,
         reinterpret_cast<const void*>(background.stack_begin),
         background.stack_size);
  return true;
}

void capture_background(TrapFrame& frame, u8 wait_socket) {
  background.frame = frame;
  background.brk = process.brk;
  background.mmap_begin = process.mmap_begin;
  background.mmap_cursor = process.mmap_cursor;
  background.mutable_begin = process.mutable_begin;
  background.stack_begin = align_down(frame.x[2], static_cast<u32>(16));
  background.pid = process.pid;
  background.parent_pid = process.parent_pid;
  background.process_group = process.process_group;
  background.session = process.session;
  background.image = process.image;
  background.child_tid_address = child_tid_address;
  background.signals = signals;
  background.signal_frame = active_signal_frame;
  background.file_creation_mask = file_creation_mask;
  memcpy(background.current_directory, process.current_directory,
         sizeof(background.current_directory));
  memcpy(background.executable_path, process.executable_path,
         sizeof(background.executable_path));
  background.wait = BackgroundWait::socket_read;
  background.wait_socket = wait_socket;
  background.used = true;
}

[[nodiscard]] bool stack_suspended_ancestor() {
  if (!parent.active || !parent.memory_saved || !parent.snapshot.valid() ||
      background.snapshot.valid() ||
      nested_ancestor_depth == nested_ancestor_capacity) {
    return false;
  }
  const u32 level = nested_ancestor_depth;
  // Transfer ownership of the exact-sized allocation. No address-space bytes
  // are copied merely to add another fork level.
  nested_ancestors[level] = parent;
  for (u32 i = 0; i < 13; ++i) {
    nested_ancestor_descriptors[level][i] = parent_descriptors[i];
    parent_descriptors[i] = {};
  }
  for (u32 i = 0; i < 3; ++i) {
    nested_ancestor_standard_redirects[level][i] =
        parent_standard_redirects[i];
    parent_standard_redirects[i] = {};
  }
  parent = {};
  background = {};
  ++nested_ancestor_depth;
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:NESTED_CLONE_STACK depth=");
  write_u32(nested_ancestor_depth);
  write_text("\n");
#endif
  return true;
}

void reinstate_suspended_ancestor(TrapFrame& frame) {
  if (nested_ancestor_depth == 0) {
    return;
  }

  // The nested child has restored its immediate parent (the background
  // service). Keep identifying that active service so it can park again or
  // exit, then put the original shell suspension back underneath it.
  capture_background(frame, network::invalid_socket);
  background.wait = BackgroundWait::none;
  const u32 level = --nested_ancestor_depth;
  parent = nested_ancestors[level];
  nested_ancestors[level] = {};
  for (u32 i = 0; i < 13; ++i) {
    parent_descriptors[i] = nested_ancestor_descriptors[level][i];
    nested_ancestor_descriptors[level][i] = {};
  }
  for (u32 i = 0; i < 3; ++i) {
    parent_standard_redirects[i] =
        nested_ancestor_standard_redirects[level][i];
    nested_ancestor_standard_redirects[level][i] = {};
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:NESTED_CLONE_UNSTACK depth=");
  write_u32(nested_ancestor_depth);
  write_text("\n");
#endif
}

[[nodiscard]] bool park_background(TrapFrame& frame, u8 wait_socket) {
  if (!parent.active) {
#ifdef MIKOS_TRIBE_INTERACTIVE
    static bool reported_no_parent = false;
    if (!reported_no_parent) {
      reported_no_parent = true;
      write_text("MIKOS:BACKGROUND_PARK_NO_PARENT\n");
    }
#endif
    return false;
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  // Once a background service has accepted a connection, keep its image
  // resident while it waits for the peer. Swapping back to the shell here
  // would reload BusyBox from simulated SD, then reload the service for the
  // very next SSH packet. UART input is a safe preemption point only when the
  // connection is directly parented by the shell. A nested session child
  // would otherwise resume its listener, occupy the one background slot, and
  // lose the suspended shell/listener ancestry.
  const auto* waiting = network::socket_slot(wait_socket);
  if (waiting != nullptr &&
      waiting->state != network::SocketState::listening) {
    static bool reported_connection_hold = false;
    if (!reported_connection_hold) {
      reported_connection_hold = true;
      write_text("MIKOS:BACKGROUND_CONNECTION_HOLD ");
      write_u32(process.pid);
      write_text("\n");
    }
    const bool uart_can_preempt =
        process_model::connection_wait_can_yield_to_uart(
            nested_ancestor_depth);
    while (!network::socket_readable(wait_socket) &&
           (!uart_can_preempt || !drivers::uart::ready())) {
      network::poll();
    }
    if (network::socket_readable(wait_socket)) {
      return false;
    }
  }
#endif
  if (background.used && process.pid != background.pid) {
#ifdef MIKOS_TRIBE_INTERACTIVE
    write_text("MIKOS:BACKGROUND_PARK_BUSY\n");
#endif
    return false;
  }
  capture_background(frame, wait_socket);
  if (!save_background_memory()) {
#ifdef MIKOS_TRIBE_INTERACTIVE
    write_text("MIKOS:BACKGROUND_PARK_ENOMEM mutable=");
    write_u32(background.mutable_size);
    write_text(" mmap=");
    write_u32(background.mmap_size);
    write_text(" stack=");
    write_u32(background.stack_size);
    write_text("\n");
#endif
    background.used = false;
    return false;
  }
  move_active_descriptors(background_descriptors,
                          background_standard_redirects);
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:BACKGROUND_PARK ");
  write_u32(background.pid);
  write_text("\n");
#endif
  arch::set_network_timer_waiting(true);
  // The first park returns from fork. Later parks replay the syscall that had
  // blocked in the already-running service.
  const bool first_park = parent.frame.x[17] ==
                          static_cast<u32>(Syscall::clone);
  resume_parent(frame, 0, false, first_park);
  return true;
}

[[nodiscard]] bool background_ready() {
  if (!background.used || background.wait != BackgroundWait::socket_read ||
      background.wait_socket == network::invalid_socket) {
    return false;
  }
  return network::socket_readable(background.wait_socket);
}

[[nodiscard]] bool resume_background_if_ready(TrapFrame& frame) {
  if (!background_ready() || parent.active || !save_parent_descriptors()) {
    return false;
  }
  parent.frame = frame;
  parent.brk = process.brk;
  parent.mmap_begin = process.mmap_begin;
  parent.mmap_cursor = process.mmap_cursor;
  parent.mutable_begin = process.mutable_begin;
  parent.stack_begin = align_down(frame.x[2], static_cast<u32>(16));
  parent.child_pid = background.pid;
  parent.lineage = {process.pid, process.parent_pid};
  parent.child_tid_address = child_tid_address;
  parent.process_group = process.process_group;
  parent.session = process.session;
  parent.image = process.image;
  parent.signals = signals;
  parent.signal_frame = active_signal_frame;
  parent.file_creation_mask = file_creation_mask;
  memcpy(parent.current_directory, process.current_directory,
         sizeof(parent.current_directory));
  memcpy(parent.executable_path, process.executable_path,
         sizeof(parent.executable_path));
  parent.active = true;
  parent.memory_saved = false;
  if (!save_parent_memory()) {
    parent.active = false;
    discard_parent_descriptors();
    return false;
  }
  reset_descriptors();
  if (!restore_executable_image(background.executable_path)) {
    write_text("MIKOS:BACKGROUND_IMAGE_RESTORE_FAIL\n");
    shutdown(9);
  }
  const auto* snapshot =
      reinterpret_cast<const u8*>(background.snapshot.address);
  u32 cursor = 0;
  memcpy(reinterpret_cast<void*>(background.mutable_begin),
         snapshot + cursor, background.mutable_size);
  cursor += background.mutable_size;
  memcpy(reinterpret_cast<void*>(background.mmap_begin),
         snapshot + cursor,
         background.mmap_size);
  cursor += background.mmap_size;
  memcpy(reinterpret_cast<void*>(background.stack_begin),
         snapshot + cursor, background.stack_size);
  release_snapshot(background.snapshot);
  move_saved_descriptors(background_descriptors,
                         background_standard_redirects);
  frame = background.frame;
  process.brk = background.brk;
  process.mmap_begin = background.mmap_begin;
  process.mmap_cursor = background.mmap_cursor;
  process.mutable_begin = background.mutable_begin;
  process.pid = background.pid;
  process.parent_pid = background.parent_pid;
  process.process_group = background.process_group;
  process.session = background.session;
  process.image = background.image;
  child_tid_address = background.child_tid_address;
  signals = background.signals;
  active_signal_frame = background.signal_frame;
  file_creation_mask = background.file_creation_mask;
  memcpy(process.current_directory, background.current_directory,
         sizeof(process.current_directory));
  memcpy(process.executable_path, background.executable_path,
         sizeof(process.executable_path));
  process.image_replaced = true;
  background.wait = BackgroundWait::none;
  arch::set_network_timer_waiting(false);
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:BACKGROUND_RESUME ");
  write_u32(background.pid);
  write_text("\n");
#endif
  return true;
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

[[nodiscard]] bool canonicalize_path(u32 address, const char* base,
                                     char* output, u32 capacity) {
  char user_path[path::capacity]{};
  return copy_user_string(address, user_path, sizeof(user_path)) &&
         path::canonicalize(base, user_path, output, capacity);
}

[[nodiscard]] Node path_node_from(u32 address, const char* base,
                                  char* canonical = nullptr) {
  char local[256]{};
  char* path = canonical == nullptr ? local : canonical;
  if (!canonicalize_path(address, base, path, 256)) {
    return Node::none;
  }
  const u32 pty_number =
      path::numbered_suffix(path, "/dev/pts/", 4);
  if (pty_number != path::invalid_number &&
      ptys.active(static_cast<u8>(pty_number))) {
    return Node{Node::pty, static_cast<u8>(pty_number)};
  }
  const auto pseudo_node = PseudoFilesystem::lookup(path);
  if (pseudo_node != PseudoFilesystem::Node::none) {
    return Node{pseudo_node};
  }
  const auto result = drivers::fs::root::lookup(path);
  return result ? Node{result.value} : Node{Node::none};
}

[[nodiscard]] Node path_node(u32 address, char* canonical = nullptr) {
  return path_node_from(address, process.current_directory, canonical);
}

[[nodiscard]] bool directory(Node node);
[[nodiscard]] u32 node_size(Node node);

[[nodiscard]] Node path_node_at(u32 descriptor, u32 address,
                                char* canonical = nullptr) {
  constexpr u32 at_current_working_directory = static_cast<u32>(-100);
  char raw[path::capacity]{};
  if (!copy_user_string(address, raw, sizeof(raw))) {
    return Node::none;
  }
  if (raw[0] == '/' || descriptor == at_current_working_directory) {
    return path_node(address, canonical);
  }
  if (descriptor < 3 || descriptor >= 16) {
    return Node::none;
  }
  const auto& slot = descriptors[descriptor - 3];
  if (!directory(slot.node)) {
    return Node::none;
  }
  return path_node_from(address, slot.path, canonical);
}

[[nodiscard]] bool directory(Node node) {
  return (node == Node::filesystem && node.file.directory()) ||
         (node == Node::pseudo &&
          PseudoFilesystem::directory(node.pseudo_node));
}

[[nodiscard]] i32 filesystem_error(drivers::fs::Error value) {
  using drivers::fs::Error;
  switch (value) {
    case Error::none:
      return 0;
    case Error::not_found:
      return error(Errno::no_entry);
    case Error::not_directory:
      return error(Errno::not_directory);
    case Error::already_exists:
      return error(Errno::file_exists);
    case Error::no_space:
      return error(Errno::no_space);
    case Error::invalid_argument:
      return error(Errno::invalid_argument);
    case Error::unsupported:
      return error(Errno::read_only_filesystem);
    default:
      return error(Errno::io);
  }
}

[[nodiscard]] i32 mkdirat(u32 directory_descriptor, u32 address, u32 mode) {
  constexpr u32 at_current_working_directory = static_cast<u32>(-100);
  char raw[path::capacity]{};
  if (!copy_user_string(address, raw, sizeof(raw))) {
    return error(Errno::bad_address);
  }
  const char* base = process.current_directory;
  if (raw[0] != '/' &&
      directory_descriptor != at_current_working_directory) {
    if (directory_descriptor < 3 || directory_descriptor >= 16 ||
        descriptors[directory_descriptor - 3].node == Node::none) {
      return error(Errno::bad_file_descriptor);
    }
    const auto& slot = descriptors[directory_descriptor - 3];
    if (!directory(slot.node)) {
      return error(Errno::not_directory);
    }
    base = slot.path;
  }
  char canonical[path::capacity]{};
  if (!path::canonicalize(base, raw, canonical, sizeof(canonical))) {
    return error(Errno::invalid_argument);
  }
  if (PseudoFilesystem::mounted(canonical)) {
    return error(Errno::read_only_filesystem);
  }
  return filesystem_error(drivers::fs::root::mkdir(
      canonical,
      static_cast<u16>((mode & 07777) & ~file_creation_mask)));
}

[[nodiscard]] i32 umask(u32 mask) {
  const u32 previous = file_creation_mask;
  file_creation_mask = mask & 0777;
  return static_cast<i32>(previous);
}

[[nodiscard]] i32 fchmodat(u32 directory_descriptor, u32 address, u32 mode) {
  const Node node = path_node_at(directory_descriptor, address);
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  if (node != Node::pty || node.pty_number == 0xff) {
    return error(Errno::read_only_filesystem);
  }
  return ptys.set_mode(node.pty_number, static_cast<u16>(mode)) ==
                 process_model::PtyStatus::success
             ? 0
             : error(Errno::no_entry);
}

[[maybe_unused, nodiscard]] i32 openat(u32 directory_descriptor, u32 path,
                                      u32 flags) {
  constexpr u32 access_mode = 3;
  constexpr u32 create = 0x40;
  constexpr u32 exclusive = 0x80;
  constexpr u32 truncate = 0x200;
  constexpr u32 append = 0x400;
  char canonical[path::capacity]{};
  Node node = path_node_at(directory_descriptor, path, canonical);
  const bool controlling_pty_path = text_is(canonical, "/dev/tty");
  bool pty_path = text_is(canonical, "/dev/ptmx") || controlling_pty_path;
  bool pty_slave = false;
  u8 pty_number = 0;
  if (!pty_path) {
    const u32 number =
        path::numbered_suffix(canonical, "/dev/pts/", 4);
    if (number != path::invalid_number) {
      pty_path = true;
      pty_slave = true;
      pty_number = static_cast<u8>(number);
    }
  }
  if (controlling_pty_path) {
    pty_slave = true;
  }
  if (pty_path) {
    u32 descriptor = 3;
    while (descriptor < 16 &&
           descriptors[descriptor - 3].node != Node::none) {
      ++descriptor;
    }
    if (descriptor == 16) {
      return error(Errno::no_memory);
    }
    const auto opened = controlling_pty_path
                            ? ptys.open_controlling_slave(process.session)
                        : pty_slave ? ptys.open_slave(pty_number)
                                    : ptys.open_master();
    if (opened.status != process_model::PtyStatus::success) {
      return opened.status == process_model::PtyStatus::no_space
                 ? error(Errno::no_memory)
                 : (opened.status == process_model::PtyStatus::locked
                        ? error(Errno::access_denied)
                        : error(Errno::no_entry));
    }
    auto& slot = descriptors[descriptor - 3];
    slot.node = Node::pty;
    slot.flags = flags;
    slot.pty = opened.handle;
    slot.pty_end = pty_slave ? process_model::PtyEnd::slave
                             : process_model::PtyEnd::master;
    memcpy(slot.path, canonical, sizeof(canonical));
    return static_cast<i32>(descriptor);
  }
  if (node == Node::none && (flags & create) != 0) {
    if (PseudoFilesystem::mounted(canonical)) {
      return error(Errno::read_only_filesystem);
    }
    if (drivers::fs::root::create(canonical, nullptr, 0) !=
        drivers::fs::Error::none) {
      return error(Errno::io);
    }
    const auto created = drivers::fs::root::lookup(canonical);
    if (created) {
      node = Node{created.value};
    }
  } else if (node != Node::none && (flags & create) != 0 &&
             (flags & exclusive) != 0) {
    return error(Errno::access_denied);
  }
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  constexpr u32 require_directory = 0x10000;
  if ((flags & require_directory) != 0 && !directory(node)) {
    return error(Errno::not_directory);
  }
  if ((flags & truncate) != 0) {
    if (node == Node::pseudo) {
      return error(Errno::read_only_filesystem);
    }
    if (node != Node::filesystem || directory(node)) {
      return error(Errno::is_directory);
    }
    if (drivers::fs::root::truncate(node.file, 0) !=
        drivers::fs::Error::none) {
      return error(Errno::io);
    }
  }
  for (u32 descriptor = 3; descriptor < 16; ++descriptor) {
    auto& slot = descriptors[descriptor - 3];
    if (slot.node == Node::none) {
      slot.node = node;
      slot.offset = (flags & append) != 0 ? node_size(node) : 0;
      slot.flags = flags & (access_mode | append);
      memcpy(slot.path, canonical, sizeof(canonical));
      return static_cast<i32>(descriptor);
    }
  }
  return error(Errno::no_memory);
}

[[maybe_unused, nodiscard]] i32 close(u32 descriptor) {
  if (descriptor < 3) {
    release_descriptor(standard_redirects[descriptor]);
    return 0;
  }
  if (descriptor < 3 || descriptor >= 16 ||
      descriptors[descriptor - 3].node == Node::none) {
    return error(Errno::bad_file_descriptor);
  }
  auto& slot = descriptors[descriptor - 3];
#ifdef MIKOS_TRIBE_INTERACTIVE
  if (slot.node == Node::network_socket) {
    write_text("MIKOS:TCP_CLOSE fd=");
    write_u32(descriptor);
    write_text(" handle=");
    write_u32(slot.socket);
    write_text("\n");
  }
#endif
  release_descriptor(slot);
  return 0;
}

[[nodiscard]] Descriptor* descriptor_slot(u32 descriptor) {
  if (descriptor < 3) {
    return standard_redirects[descriptor].node == Node::none
               ? nullptr
               : &standard_redirects[descriptor];
  }
  return descriptor < 16 ? &descriptors[descriptor - 3] : nullptr;
}

[[nodiscard]] bool descriptor_open(u32 descriptor) {
  return descriptor < 3 ||
         (descriptor < 16 &&
          descriptors[descriptor - 3].node != Node::none);
}

[[nodiscard]] bool descriptor_read_ready(u32 descriptor) {
  if (descriptor == 0 && standard_redirects[0].node == Node::none) {
    return drivers::uart::ready();
  }
  const Descriptor* slot = descriptor_slot(descriptor);
  if (slot == nullptr) {
    return false;
  }
  if (slot->node == Node::network_socket) {
    return network::socket_readable(slot->socket);
  }
  if (slot->node == Node::pipe) {
    return slot->pipe_end == process_model::PipeEnd::read &&
           pipes.readable(slot->pipe);
  }
  if (slot->node == Node::pty) {
    return ptys.readable(slot->pty, slot->pty_end);
  }
  if (slot->node == Node::standard_input) {
    return drivers::uart::ready();
  }
  return (slot->node == Node::filesystem || slot->node == Node::pseudo) &&
         !directory(slot->node);
}

[[nodiscard]] bool descriptor_write_ready(u32 descriptor) {
  if ((descriptor == 1 || descriptor == 2) &&
      standard_redirects[descriptor].node == Node::none) {
    return true;
  }
  const Descriptor* slot = descriptor_slot(descriptor);
  if (slot == nullptr) {
    return false;
  }
  if (slot->node == Node::network_socket) {
    return network::socket_writable(slot->socket);
  }
  if (slot->node == Node::pipe) {
    return slot->pipe_end == process_model::PipeEnd::write &&
           pipes.writable(slot->pipe);
  }
  if (slot->node == Node::pty) {
    return ptys.writable(slot->pty, slot->pty_end);
  }
  if (slot->node == Node::standard_output) {
    return true;
  }
  return slot->node == Node::filesystem && !directory(slot->node) &&
         (slot->flags & 3) != 0;
}

[[nodiscard]] bool descriptor_error_ready(u32 descriptor) {
  const Descriptor* slot = descriptor_slot(descriptor);
  if (slot != nullptr && slot->node == Node::pipe) {
    return pipes.hung_up(slot->pipe, slot->pipe_end);
  }
  if (slot != nullptr && slot->node == Node::pty) {
    // A closed PTY peer is readable EOF (and may still have buffered bytes),
    // not select(2) exceptional data. Reporting it in exceptfds makes
    // Dropbear tear down the channel before draining the final command output.
    return false;
  }
  if (slot == nullptr || slot->node != Node::network_socket) {
    return false;
  }
  const auto* socket = network::socket_slot(slot->socket);
  return socket != nullptr && socket->state == network::SocketState::reset;
}

[[nodiscard]] i32 socket(u32 domain, u32 type, u32 protocol) {
  using abi::socket::ValidationResult;
  switch (abi::socket::validate(domain, type, protocol)) {
    case ValidationResult::address_family_not_supported:
      return error(Errno::address_family_not_supported);
    case ValidationResult::socket_type_not_supported:
      return error(Errno::operation_not_supported);
    case ValidationResult::protocol_not_supported:
      return error(Errno::protocol_not_supported);
    case ValidationResult::success:
      break;
  }
  const auto opened = network::socket_open(abi::socket::type(type));
  if (opened.result != network::SocketResult::success) {
    return error(Errno::no_memory);
  }
  for (u32 descriptor = 3; descriptor < 16; ++descriptor) {
    auto& slot = descriptors[descriptor - 3];
    if (slot.node == Node::none) {
      slot.node = Node::network_socket;
      slot.flags = type & abi::socket::sock_nonblock;
      slot.socket = opened.handle;
      return static_cast<i32>(descriptor);
    }
  }
  static_cast<void>(network::socket_close(opened.handle));
  return error(Errno::no_memory);
}

[[nodiscard]] i32 socket_error(network::SocketResult result) {
  using network::SocketResult;
  switch (result) {
    case SocketResult::success:
    case SocketResult::end_of_file:
      return 0;
    case SocketResult::bad_handle:
      return error(Errno::bad_file_descriptor);
    case SocketResult::wrong_type:
      return error(Errno::operation_not_supported);
    case SocketResult::invalid_argument:
      return error(Errno::invalid_argument);
    case SocketResult::address_in_use:
      return error(Errno::address_in_use);
    case SocketResult::not_bound:
    case SocketResult::not_listening:
      return error(Errno::invalid_argument);
    case SocketResult::would_block:
      return error(Errno::try_again);
    case SocketResult::not_connected:
      return error(Errno::not_connected);
    case SocketResult::no_space:
      return error(Errno::no_memory);
    case SocketResult::reset:
      return error(Errno::io);
  }
  return error(Errno::io);
}

[[nodiscard]] Descriptor* network_descriptor(u32 descriptor) {
  Descriptor* value = descriptor_slot(descriptor);
  return value != nullptr && value->node == Node::network_socket ? value
                                                                 : nullptr;
}

[[nodiscard]] bool read_sockaddr(u32 address, u32 size,
                                 network::Endpoint& endpoint) {
  if (size < sizeof(abi::socket::SockaddrIn) ||
      !user_memory.contains(address, sizeof(abi::socket::SockaddrIn))) {
    return false;
  }
  abi::socket::SockaddrIn value{};
  memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
  if (value.family != abi::socket::af_inet) {
    return false;
  }
  endpoint.port = net16(value.port);
  copy_octets(endpoint.address.octet, value.address);
  return true;
}

[[nodiscard]] bool sockaddr_output_valid(u32 address, u32 length_address) {
  if (address == 0) {
    return true;
  }
  return length_address != 0 &&
         user_memory.contains(length_address, sizeof(u32)) &&
         user_memory.contains(address, sizeof(abi::socket::SockaddrIn));
}

[[nodiscard]] i32 write_sockaddr(network::Endpoint endpoint, u32 address,
                                 u32 length_address) {
  if (address == 0) {
    return 0;
  }
  if (!sockaddr_output_valid(address, length_address)) {
    return error(Errno::bad_address);
  }
  const u32 capacity = *reinterpret_cast<const u32*>(length_address);
  if (capacity < sizeof(abi::socket::SockaddrIn)) {
    return error(Errno::invalid_argument);
  }
  abi::socket::SockaddrIn value{};
  value.family = abi::socket::af_inet;
  value.port = net16(endpoint.port);
  copy_octets(value.address, endpoint.address.octet);
  memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  *reinterpret_cast<u32*>(length_address) = sizeof(value);
  return 0;
}

[[nodiscard]] i32 bind(u32 descriptor, u32 address, u32 size) {
  auto* slot = network_descriptor(descriptor);
  if (slot == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  network::Endpoint endpoint{};
  if (!read_sockaddr(address, size, endpoint)) {
    return error(Errno::invalid_argument);
  }
  return socket_error(network::socket_bind(slot->socket, endpoint));
}

[[nodiscard]] i32 listen(u32 descriptor, u32 backlog) {
  auto* slot = network_descriptor(descriptor);
  if (slot == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  const auto result = network::socket_listen(slot->socket, backlog);
#ifdef MIKOS_TRIBE_INTERACTIVE
  if (result == network::SocketResult::success) {
    const auto* socket = network::socket_slot(slot->socket);
    write_text("MIKOS:TCP_LISTEN ");
    write_u32(socket == nullptr ? 0 : socket->local.port);
    write_text("\n");
  }
#endif
  return socket_error(result);
}

[[nodiscard]] i32 accept(TrapFrame& frame, u32 descriptor, u32 address,
                         u32 length_address, u32 flags) {
  constexpr u32 supported_flags = abi::socket::sock_nonblock |
                                  abi::socket::sock_cloexec;
  if ((flags & ~supported_flags) != 0) {
    return error(Errno::invalid_argument);
  }
  auto* listening = network_descriptor(descriptor);
  if (listening == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  if (!sockaddr_output_valid(address, length_address)) {
    return error(Errno::bad_address);
  }
  u32 accepted_descriptor = 3;
  while (accepted_descriptor < 16 &&
         descriptors[accepted_descriptor - 3].node != Node::none) {
    ++accepted_descriptor;
  }
  if (accepted_descriptor == 16) {
    return error(Errno::no_memory);
  }
  network::AcceptResult accepted{};
  for (;;) {
    accepted = network::socket_accept(listening->socket);
    if (accepted.result != network::SocketResult::would_block) {
      break;
    }
    if ((listening->flags & abi::socket::sock_nonblock) != 0 ||
        (flags & abi::socket::sock_nonblock) != 0) {
      return error(Errno::try_again);
    }
    if (park_background(frame, listening->socket)) {
      return static_cast<i32>(frame.x[10]);
    }
    network::poll();
  }
  if (accepted.result != network::SocketResult::success) {
    return socket_error(accepted.result);
  }
  const i32 address_result =
      write_sockaddr(accepted.peer, address, length_address);
  if (address_result != 0) {
    static_cast<void>(network::socket_close(accepted.handle));
    return address_result;
  }
  auto& result = descriptors[accepted_descriptor - 3];
  result.node = Node::network_socket;
  result.flags = flags & abi::socket::sock_nonblock;
  result.socket = accepted.handle;
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:TCP_ACCEPT fd=");
  write_u32(accepted_descriptor);
  write_text(" buffered=");
  const auto* accepted_socket = network::socket_slot(accepted.handle);
  write_u32(accepted_socket == nullptr ? 0 : accepted_socket->receive_size);
  write_text("\n");
#endif
  return static_cast<i32>(accepted_descriptor);
}

[[nodiscard]] i32 socket_name(u32 descriptor, u32 address,
                              u32 length_address, bool peer) {
  auto* slot = network_descriptor(descriptor);
  if (slot == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  const auto* socket = network::socket_slot(slot->socket);
  if (socket == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  if (peer && socket->state != network::SocketState::established &&
      socket->state != network::SocketState::close_wait &&
      socket->state != network::SocketState::reset) {
    return error(Errno::not_connected);
  }
  return write_sockaddr(peer ? socket->remote : socket->local, address,
                        length_address);
}

[[nodiscard]] i32 setsockopt(u32 descriptor, u32 value_address,
                             u32 value_size) {
  if (network_descriptor(descriptor) == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  if (value_size != 0 &&
      !user_memory.contains(value_address, value_size)) {
    return error(Errno::bad_address);
  }
  // The bounded stack has no tunable buffers. SO_REUSEADDR, TCP_NODELAY,
  // IP_TOS, keepalive, and similar Dropbear hints are accepted as no-ops.
  return 0;
}

[[nodiscard]] i32 shutdown_socket(u32 descriptor, u32 how) {
  auto* slot = network_descriptor(descriptor);
  return slot == nullptr
             ? error(Errno::bad_file_descriptor)
             : socket_error(network::socket_shutdown(slot->socket, how));
}

[[nodiscard]] i32 duplicate_to(u32 old_descriptor, u32 new_descriptor,
                               u32 flags) {
  if (flags != 0 || old_descriptor >= 16 || new_descriptor >= 16 ||
      old_descriptor == new_descriptor) {
    return error(Errno::invalid_argument);
  }
  Descriptor source{};
  if (old_descriptor == 0) {
    source.node = Node::standard_input;
  } else if (old_descriptor == 1 || old_descriptor == 2) {
    source.node = Node::standard_output;
  } else if (descriptors[old_descriptor - 3].node != Node::none) {
    source = descriptors[old_descriptor - 3];
  } else {
    return error(Errno::bad_file_descriptor);
  }
  if (new_descriptor < 3) {
    if (source.node == Node::network_socket &&
        network::socket_retain(source.socket) !=
            network::SocketResult::success) {
      return error(Errno::bad_file_descriptor);
    }
    if (source.node == Node::pipe &&
        pipes.retain(source.pipe, source.pipe_end) !=
            process_model::PipeStatus::success) {
      if (source.node == Node::network_socket) {
        static_cast<void>(network::socket_close(source.socket));
      }
      return error(Errno::bad_file_descriptor);
    }
    if (source.node == Node::pty &&
        ptys.retain(source.pty, source.pty_end) !=
            process_model::PtyStatus::success) {
      return error(Errno::bad_file_descriptor);
    }
    release_descriptor(standard_redirects[new_descriptor]);
    standard_redirects[new_descriptor] = source;
  } else {
    if (source.node == Node::network_socket &&
        network::socket_retain(source.socket) !=
            network::SocketResult::success) {
      return error(Errno::bad_file_descriptor);
    }
    if (source.node == Node::pipe &&
        pipes.retain(source.pipe, source.pipe_end) !=
            process_model::PipeStatus::success) {
      if (source.node == Node::network_socket) {
        static_cast<void>(network::socket_close(source.socket));
      }
      return error(Errno::bad_file_descriptor);
    }
    if (source.node == Node::pty &&
        ptys.retain(source.pty, source.pty_end) !=
            process_model::PtyStatus::success) {
      return error(Errno::bad_file_descriptor);
    }
    release_descriptor(descriptors[new_descriptor - 3]);
    descriptors[new_descriptor - 3] = source;
  }
  return static_cast<i32>(new_descriptor);
}

[[nodiscard]] i32 pipe2(u32 address, u32 flags) {
  constexpr u32 nonblock = 0x800;
  constexpr u32 cloexec = 0x80000;
  if ((flags & ~(nonblock | cloexec)) != 0) {
    return error(Errno::invalid_argument);
  }
  if (!user_memory.contains(address, 2 * sizeof(u32)) ||
      !user_memory.aligned(address, alignof(u32))) {
    return error(Errno::bad_address);
  }
  u32 slots[2]{};
  u32 found = 0;
  for (u32 descriptor = 3; descriptor < 16 && found != 2; ++descriptor) {
    if (descriptors[descriptor - 3].node == Node::none) {
      slots[found++] = descriptor;
    }
  }
  if (found != 2) {
    return error(Errno::no_memory);
  }
  const auto created = pipes.create();
  if (created.status != process_model::PipeStatus::success) {
    return error(Errno::no_memory);
  }
  auto& reader = descriptors[slots[0] - 3];
  reader.node = Node::pipe;
  reader.flags = flags;
  reader.pipe = created.handle;
  reader.pipe_end = process_model::PipeEnd::read;
  auto& writer = descriptors[slots[1] - 3];
  writer.node = Node::pipe;
  writer.flags = flags;
  writer.pipe = created.handle;
  writer.pipe_end = process_model::PipeEnd::write;
  auto* output = reinterpret_cast<u32*>(address);
  output[0] = slots[0];
  output[1] = slots[1];
  return 0;
}

[[nodiscard]] i32 fcntl(u32 descriptor, u32 command, u32 argument) {
  constexpr u32 duplicate = 0;
  constexpr u32 get_descriptor_flags = 1;
  constexpr u32 set_descriptor_flags = 2;
  constexpr u32 get_status_flags = 3;
  constexpr u32 set_status_flags = 4;
  constexpr u32 duplicate_cloexec = 1030;
  if (command == get_descriptor_flags || command == set_descriptor_flags) {
    return descriptor_open(descriptor) ? 0
                                       : error(Errno::bad_file_descriptor);
  }
  if (command == get_status_flags) {
    if (descriptor < 3) {
      return descriptor == 0 ? 0 : 1;
    }
    return descriptor < 16 && descriptors[descriptor - 3].node != Node::none
               ? static_cast<i32>(descriptors[descriptor - 3].flags)
               : error(Errno::bad_file_descriptor);
  }
  if (command == set_status_flags) {
    if (descriptor < 3) {
      return 0;
    }
    if (descriptor >= 16 || descriptors[descriptor - 3].node == Node::none) {
      return error(Errno::bad_file_descriptor);
    }
    descriptors[descriptor - 3].flags = argument;
    return 0;
  }
  if (command != duplicate && command != duplicate_cloexec) {
    return error(Errno::invalid_argument);
  }
  if (!descriptor_open(descriptor)) {
    return error(Errno::bad_file_descriptor);
  }
  for (u32 target = argument < 3 ? 3 : argument; target < 16; ++target) {
    if (descriptors[target - 3].node == Node::none) {
      return duplicate_to(descriptor, target, 0);
    }
  }
  return error(Errno::no_memory);
}

[[nodiscard]] i32 unlinkat(u32 directory_descriptor, u32 path_address,
                           u32 flags) {
  if (flags != 0) {
    return error(Errno::invalid_argument);
  }
  char canonical[path::capacity]{};
  const Node node =
      path_node_at(directory_descriptor, path_address, canonical);
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  if (node == Node::pseudo) {
    return error(Errno::read_only_filesystem);
  }
  return drivers::fs::root::remove(canonical) == drivers::fs::Error::none
             ? 0
             : error(Errno::io);
}

[[nodiscard]] i32 renameat(u32 old_directory, u32 old_path,
                           u32 new_directory, u32 new_path) {
  char source[path::capacity]{};
  char destination[path::capacity]{};
  const Node source_node = path_node_at(old_directory, old_path, source);
  if (source_node == Node::none) {
    return error(Errno::no_entry);
  }
  if (source_node == Node::pseudo) {
    return error(Errno::read_only_filesystem);
  }
  char raw[path::capacity]{};
  if (!copy_user_string(new_path, raw, sizeof(raw))) {
    return error(Errno::bad_address);
  }
  const char* base = process.current_directory;
  if (raw[0] != '/' &&
      new_directory != static_cast<u32>(-100)) {
    if (new_directory < 3 || new_directory >= 16 ||
        !directory(descriptors[new_directory - 3].node)) {
      return error(Errno::bad_file_descriptor);
    }
    base = descriptors[new_directory - 3].path;
  }
  if (!path::canonicalize(base, raw, destination, sizeof(destination))) {
    return error(Errno::invalid_argument);
  }
  if (PseudoFilesystem::mounted(destination)) {
    return error(Errno::read_only_filesystem);
  }
  return drivers::fs::root::move(source, destination) ==
                 drivers::fs::Error::none
             ? 0
             : error(Errno::io);
}

[[nodiscard]] i32 ftruncate(u32 descriptor, u32 size) {
  if (descriptor < 3 || descriptor >= 16) {
    return error(Errno::bad_file_descriptor);
  }
  auto& slot = descriptors[descriptor - 3];
  if (slot.node != Node::filesystem || (slot.flags & 3) == 0) {
    return error(Errno::bad_file_descriptor);
  }
  return drivers::fs::root::truncate(slot.node.file, size) ==
                 drivers::fs::Error::none
             ? 0
             : error(Errno::io);
}

[[nodiscard]] i32 truncate_path(u32 path_address, u32 size) {
  char canonical[path::capacity]{};
  Node node = path_node(path_address, canonical);
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  if (node == Node::pseudo) {
    return error(Errno::read_only_filesystem);
  }
  if (node != Node::filesystem || directory(node)) {
    return error(Errno::is_directory);
  }
  return drivers::fs::root::truncate(node.file, size) ==
                 drivers::fs::Error::none
             ? 0
             : error(Errno::io);
}

[[nodiscard]] const char* node_contents(Node node) {
  if (node == Node::pseudo &&
      node.pseudo_node == PseudoFilesystem::Node::proc_net_tcp) {
    return network::tcp_table();
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  if (node == Node::pseudo && background.used && background.pid == 2 &&
      text_is(background.executable_path, "/usr/sbin/dropbear")) {
    static constexpr const char dropbear_stat[] =
        "2 (dropbear) S 1 1 1 0 0 0 0 0 0 0 2 1 0 0 20 0 1 0 1 "
        "2600000 144 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    static constexpr const char dropbear_command[] =
        "/usr/sbin/dropbear\0";
    if (node.pseudo_node == PseudoFilesystem::Node::proc_pid2_stat) {
      return dropbear_stat;
    }
    if (node.pseudo_node == PseudoFilesystem::Node::proc_pid2_cmdline) {
      return dropbear_command;
    }
  }
#endif
  return node == Node::pseudo
             ? PseudoFilesystem::contents(node.pseudo_node)
             : nullptr;
}

[[nodiscard]] u32 node_size(Node node) {
  if (node == Node::filesystem) {
    return node.file.size > 0xffffffffu
               ? 0xffffffffu
               : static_cast<u32>(node.file.size);
  }
  if (node == Node::pseudo) {
    if (node.pseudo_node == PseudoFilesystem::Node::proc_net_tcp) {
      return text_size(network::tcp_table());
    }
#ifdef MIKOS_TRIBE_INTERACTIVE
    if (node.pseudo_node == PseudoFilesystem::Node::proc_pid2_cmdline &&
        background.used && background.pid == 2 &&
        text_is(background.executable_path, "/usr/sbin/dropbear")) {
      return sizeof("/usr/sbin/dropbear");
    }
#endif
    if (node.pseudo_node == PseudoFilesystem::Node::proc_pid2_stat) {
      return text_size(node_contents(node));
    }
    return PseudoFilesystem::size(node.pseudo_node);
  }
  const char* contents = node_contents(node);
  return contents == nullptr ? 0 : text_size(contents);
}

[[nodiscard]] i32 read_virtual(TrapFrame& frame, u32 descriptor, u32 address,
                               u32 size) {
  Descriptor* selected = descriptor_slot(descriptor);
  if (selected == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  auto& slot = *selected;
  if (directory(slot.node)) {
    return error(Errno::is_directory);
  }
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  if (slot.node == Node::network_socket) {
    if (size == 0) {
      return 0;
    }
    for (;;) {
      const auto result = network::socket_read(
          slot.socket, reinterpret_cast<u8*>(address), size);
      if (result.result == network::SocketResult::success) {
#ifdef MIKOS_TRIBE_INTERACTIVE
        static u32 tcp_read_calls = 0;
        ++tcp_read_calls;
        if (tcp_read_calls == 1) {
          write_text("MIKOS:TCP_READ ");
          write_u32(result.size);
          write_text("\n");
        }
        if ((tcp_read_calls % 8) == 0 || result.size > 1) {
          write_text("MIKOS:TCP_READ_PROGRESS calls=");
          write_u32(tcp_read_calls);
          write_text(" size=");
          write_u32(result.size);
          write_text(" remaining=");
          const auto* socket = network::socket_slot(slot.socket);
          write_u32(socket == nullptr ? 0 : socket->receive_size);
          write_text("\n");
        }
#endif
        return static_cast<i32>(result.size);
      }
      if (result.result == network::SocketResult::end_of_file) {
        return 0;
      }
      if (result.result != network::SocketResult::would_block) {
        return socket_error(result.result);
      }
      if ((slot.flags & abi::socket::sock_nonblock) != 0) {
        return error(Errno::try_again);
      }
      if (park_background(frame, slot.socket)) {
        return static_cast<i32>(frame.x[10]);
      }
      network::poll();
      if (resume_interactive_child_if_ready(frame)) {
        return static_cast<i32>(frame.x[10]);
      }
    }
  }
  if (slot.node == Node::pipe) {
    if (slot.pipe_end != process_model::PipeEnd::read) {
      return error(Errno::bad_file_descriptor);
    }
    for (;;) {
      const auto result = pipes.read(
          slot.pipe, reinterpret_cast<u8*>(address), size);
      if (result.status == process_model::PipeStatus::success) {
        return static_cast<i32>(result.size);
      }
      if (result.status == process_model::PipeStatus::end_of_file) {
        return 0;
      }
      if (result.status != process_model::PipeStatus::would_block) {
        return error(Errno::bad_file_descriptor);
      }
      constexpr u32 nonblock = 0x800;
      if ((slot.flags & nonblock) != 0) {
        return error(Errno::try_again);
      }
      // The process scheduler phase will park and replay this syscall. Returning
      // EAGAIN here is the bounded interim behavior and avoids a kernel spin.
      return error(Errno::try_again);
    }
  }
  if (slot.node == Node::pty) {
    const auto result =
        ptys.read(slot.pty, slot.pty_end, reinterpret_cast<u8*>(address), size);
#ifdef MIKOS_TRIBE_INTERACTIVE
    static u32 pty_read_reports = 0;
    if (pty_read_reports < 16 &&
        (result.status != process_model::PtyStatus::would_block ||
         result.size != 0)) {
      ++pty_read_reports;
      write_text("MIKOS:PTY_READ pid=");
      write_u32(process.pid);
      write_text(" fd=");
      write_u32(descriptor);
      write_text(" end=");
      write_text(slot.pty_end == process_model::PtyEnd::master ? "master"
                                                               : "slave");
      write_text(" status=");
      write_u32(static_cast<u32>(result.status));
      write_text(" size=");
      write_u32(result.size);
      write_text("\n");
    }
#endif
    switch (result.status) {
      case process_model::PtyStatus::success:
        return static_cast<i32>(result.size);
      case process_model::PtyStatus::end_of_file:
        return 0;
      case process_model::PtyStatus::would_block: {
        constexpr u32 nonblock = 0x800;
        if ((slot.flags & nonblock) == 0) {
          const auto wait_pty = slot.pty;
          const auto wait_end = slot.pty_end;
          if (park_interactive_child(frame, wait_pty, wait_end)) {
            return static_cast<i32>(frame.x[10]);
          }
        }
        return error(Errno::try_again);
      }
      case process_model::PtyStatus::io_error:
        return error(Errno::io);
      default:
        return error(Errno::bad_file_descriptor);
    }
  }
  const u32 available = node_size(slot.node);
  if (slot.offset >= available) {
    return 0;
  }
  u32 count = available - slot.offset;
  if (count > size) {
    count = size;
  }
  if (slot.node == Node::filesystem) {
    const auto result = drivers::fs::root::read(
        slot.node.file, slot.offset, reinterpret_cast<u8*>(address), count);
    if (!result) {
      return error(Errno::io);
    }
    count = result.value;
  } else {
    const char* contents = node_contents(slot.node);
    if (contents == nullptr) {
      return error(Errno::bad_file_descriptor);
    }
    memcpy(reinterpret_cast<void*>(address), contents + slot.offset, count);
  }
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

struct Ext4DirectoryState {
  u32 address;
  u32 capacity;
  u32 start;
  u32 cursor;
  u32 next;
  u32 total;
  bool root;
  bool full;
};

[[nodiscard]] bool emit_ext4_directory_entry(
    void* context, const drivers::fs::root::Entry& entry) {
  auto& state = *static_cast<Ext4DirectoryState*>(context);
  const u32 index = state.cursor++;
  if (index < state.start) {
    return true;
  }
  if (state.root &&
      (entry.name.equals("proc") || entry.name.equals("sys"))) {
    state.next = index + 1;
    return true;
  }
  constexpr u8 unknown_type = 0;
  constexpr u8 directory_type = 4;
  constexpr u8 regular_type = 8;
  constexpr u8 symbolic_link_type = 10;
  u8 type = unknown_type;
  switch (entry.node.type) {
    case drivers::fs::root::Type::directory:
      type = directory_type;
      break;
    case drivers::fs::root::Type::regular:
      type = regular_type;
      break;
    case drivers::fs::root::Type::symbolic_link:
      type = symbolic_link_type;
      break;
    case drivers::fs::root::Type::other:
      break;
  }
  const i32 result = emit_dirent(
      state.address + state.total, state.capacity - state.total,
      entry.node.inode, index + 1, type, entry.name.data);
  if (result == 0) {
    state.full = true;
    return false;
  }
  state.total += static_cast<u32>(result);
  state.next = index + 1;
  return true;
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
  constexpr u8 regular_type = 8;
  constexpr u32 overlay_start = 0xfffffff0u;
  constexpr u32 end_of_directory = 0xffffffffu;
  u32 total = 0;
  if (slot.node == Node::filesystem) {
    if (slot.offset == end_of_directory) {
      return 0;
    }
    const bool root = text_is(slot.path, "/");
    if (slot.offset < overlay_start) {
      Ext4DirectoryState state{address, size, slot.offset, 0, slot.offset,
                               0, root, false};
      const auto result = drivers::fs::root::for_each(
          slot.node.file, &state, emit_ext4_directory_entry);
      if (result != drivers::fs::Error::none) {
        return error(Errno::io);
      }
      slot.offset = state.next;
      total = state.total;
      if (state.full) {
        return static_cast<i32>(total);
      }
      slot.offset = root ? overlay_start : end_of_directory;
    }
    if (root) {
      static constexpr struct {
        const char* name;
        PseudoFilesystem::Node node;
      } overlays[] = {{"proc", PseudoFilesystem::Node::proc},
                      {"sys", PseudoFilesystem::Node::sys}};
      while (slot.offset - overlay_start < 2) {
        const u32 index = slot.offset - overlay_start;
        const i32 emitted = emit_dirent(
            address + total, size - total,
            PseudoFilesystem::inode(overlays[index].node), slot.offset + 1,
            directory_type, overlays[index].name);
        if (emitted == 0) {
          break;
        }
        total += static_cast<u32>(emitted);
        ++slot.offset;
      }
      if (slot.offset - overlay_start == 2) {
        slot.offset = end_of_directory;
      }
    }
    return static_cast<i32>(total);
  }

  const u32 entry_count =
      PseudoFilesystem::entry_count(slot.node.pseudo_node);
  while (slot.offset < entry_count) {
    const auto entry =
        PseudoFilesystem::entry(slot.node.pseudo_node, slot.offset);
    const u8 type = entry.type == PseudoFilesystem::Type::directory
                        ? directory_type
                        : regular_type;
    const i32 result = emit_dirent(address + total, size - total,
                                   PseudoFilesystem::inode(entry.node),
                                   slot.offset + 1, type, entry.name);
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
  const u32 character_mode =
      0020000 | (node.pty_number == 0xff ? 0666
                                         : ptys.mode(node.pty_number));
  constexpr u32 fifo_mode = 0010000 | 0666;
  value.inode =
      node == Node::filesystem
          ? node.file.inode
          : (node == Node::pseudo
                 ? PseudoFilesystem::inode(node.pseudo_node)
                 : static_cast<u32>(node.kind));
  value.mode = node == Node::none || node == Node::pty
                   ? character_mode
                   : (node == Node::pipe
                          ? fifo_mode
                          : (node == Node::filesystem
                                 ? node.file.mode
                                 : (directory(node) ? directory_mode
                                                    : regular_mode)));
  value.links = directory(node) ? 2 : 1;
  value.size = node_size(node);
  value.block_size = 4096;
  value.blocks = (value.size + 511) / 512;
  memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  return 0;
}

[[maybe_unused, nodiscard]] i32 fstat64(u32 descriptor, u32 address) {
  if (descriptor < 3) {
    return write_stat(Node::none, address);
  }
  if (descriptor >= 16 || descriptors[descriptor - 3].node == Node::none) {
    return error(Errno::bad_file_descriptor);
  }
  Node node = descriptors[descriptor - 3].node;
  if (node == Node::pty) {
    node.pty_number = descriptors[descriptor - 3].pty.slot;
  }
  return write_stat(node, address);
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
  const u16 character_mode = static_cast<u16>(
      0020000 | (node.pty_number == 0xff ? 0666
                                         : ptys.mode(node.pty_number)));
  Statx value{};
  value.mask = 0x7ff;
  value.block_size = 4096;
  value.links = directory(node) ? 2 : 1;
  // Keep statx() consistent with fstat64(). Static glibc's ttyname_r() uses
  // fstat64() on the open slave and statx() for the /dev/pts/N pathname, then
  // rejects the name unless both describe the same character device.
  value.mode = node == Node::pty
                   ? character_mode
                   : (node == Node::filesystem
                          ? node.file.mode
                          : (directory(node) ? directory_mode
                                             : regular_mode));
  value.inode =
      node == Node::filesystem
          ? node.file.inode
          : (node == Node::pseudo
                 ? PseudoFilesystem::inode(node.pseudo_node)
                 : static_cast<u32>(node.kind));
  value.size = node_size(node);
  value.blocks = (value.size + 511) / 512;
  memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
  return 0;
}

[[maybe_unused, nodiscard]] i32 readlinkat(u32 directory_descriptor, u32 path,
                                          u32 address, u32 size) {
  char canonical[256]{};
  static_cast<void>(path_node_at(directory_descriptor, path, canonical));
  const char* target = nullptr;
  if (text_is(canonical, "/proc/self/exe")) {
    target = process.executable_path;
  } else {
    const u32 descriptor =
        path::numbered_suffix(canonical, "/proc/self/fd/", 16);
    const Descriptor* slot =
        descriptor == path::invalid_number ? nullptr
                                           : descriptor_slot(descriptor);
    if (slot == nullptr || slot->node == Node::none || slot->path[0] == '\0') {
      return error(Errno::no_entry);
    }
    target = slot->path;
  }
  const u32 target_size = text_size(target);
  const u32 count = size < target_size ? size : target_size;
  if (!user_memory.contains(address, count)) {
    return error(Errno::bad_address);
  }
  memcpy(reinterpret_cast<void*>(address), target, count);
  return static_cast<i32>(count);
}

[[maybe_unused, nodiscard]] i32 ppoll(TrapFrame& frame, u32 address, u32 count,
                                     u32 timeout_address, bool time64) {
  if (count > 16 ||
      !user_memory.contains(address, count * sizeof(Pollfd32))) {
    return error(Errno::bad_address);
  }
  u64 deadline = ~u64{0};
  if (timeout_address != 0) {
    constexpr u64 ticks_per_second = 10'000'000;
    if (time64) {
      if (!user_memory.contains(timeout_address, sizeof(Timespec64))) {
        return error(Errno::bad_address);
      }
      const auto timeout =
          *reinterpret_cast<const Timespec64*>(timeout_address);
      deadline = arch::time_ticks() + timeout.seconds * ticks_per_second +
                 static_cast<u32>(timeout.nanoseconds) / 100;
    } else {
      if (!user_memory.contains(timeout_address, sizeof(Timespec32))) {
        return error(Errno::bad_address);
      }
      const auto timeout =
          *reinterpret_cast<const Timespec32*>(timeout_address);
      if (timeout.seconds < 0 || timeout.nanoseconds < 0) {
        return error(Errno::invalid_argument);
      }
      deadline = arch::time_ticks() +
                 static_cast<u32>(timeout.seconds) * ticks_per_second +
                 static_cast<u32>(timeout.nanoseconds) / 100;
    }
  }
  auto* poll_descriptors = reinterpret_cast<Pollfd32*>(address);
  for (;;) {
    network::poll();
    if (resume_interactive_child_if_ready(frame)) {
      return static_cast<i32>(frame.x[10]);
    }
    // The foreground interactive shell normally blocks in ppoll() before it
    // reads UART input.  A parked listener can become ready while the shell is
    // here, so give it the same restoration point as the blocking read path.
    // Without this check the TCP handshake completes, but the listener never
    // returns from accept() and the SSH peer eventually times out.
    if (resume_background_if_ready(frame)) {
      return static_cast<i32>(frame.x[10]);
    }
    i32 ready_count = 0;
    for (u32 i = 0; i < count; ++i) {
      poll_descriptors[i].returned_events = 0;
      constexpr u16 poll_input = 1;
      constexpr u16 poll_output = 4;
      constexpr u16 poll_error = 8;
      constexpr u16 poll_invalid = 0x20;
      const i32 descriptor = poll_descriptors[i].descriptor;
      if (descriptor < 0) {
        continue;
      }
      if (descriptor >= 16 ||
          (descriptor_slot(static_cast<u32>(descriptor)) == nullptr &&
           descriptor != 0 && descriptor != 1 && descriptor != 2)) {
        poll_descriptors[i].returned_events = poll_invalid;
      } else {
        const u32 value = static_cast<u32>(descriptor);
        if ((poll_descriptors[i].events & poll_input) != 0 &&
            descriptor_read_ready(value)) {
          poll_descriptors[i].returned_events |= poll_input;
        }
        if ((poll_descriptors[i].events & poll_output) != 0 &&
            descriptor_write_ready(value)) {
          poll_descriptors[i].returned_events |= poll_output;
        }
        if (descriptor_error_ready(value)) {
          poll_descriptors[i].returned_events |= poll_error;
        }
      }
      if (poll_descriptors[i].returned_events != 0) {
        ++ready_count;
      }
    }
    if (ready_count != 0 || arch::time_ticks() >= deadline) {
      return ready_count;
    }
    for (u32 i = 0; i < count; ++i) {
      constexpr u16 poll_input = 1;
      const i32 descriptor = poll_descriptors[i].descriptor;
      if (descriptor < 0 || descriptor >= 16 ||
          (poll_descriptors[i].events & poll_input) == 0) {
        continue;
      }
      const auto* slot = descriptor_slot(static_cast<u32>(descriptor));
#ifdef MIKOS_TRIBE_INTERACTIVE
      static bool reported_ppoll_wait = false;
      if (!reported_ppoll_wait && slot != nullptr &&
          slot->node == Node::network_socket) {
        reported_ppoll_wait = true;
        write_text("MIKOS:PPOLL_NETWORK_WAIT fd=");
        write_u32(static_cast<u32>(descriptor));
        write_text("\n");
      }
#endif
      if (slot != nullptr && slot->node == Node::network_socket &&
          park_background(frame, slot->socket)) {
        return static_cast<i32>(frame.x[10]);
      }
      if (slot != nullptr && slot->node == Node::pty) {
        const auto wait_pty = slot->pty;
        const auto wait_end = slot->pty_end;
        if (park_interactive_child(frame, wait_pty, wait_end)) {
          return static_cast<i32>(frame.x[10]);
        }
      }
    }
  }
}

[[maybe_unused, nodiscard]] i32 pselect6(TrapFrame& frame, u32 count,
                                        u32 read_address,
                                        u32 write_address, u32 error_address,
                                        u32 timeout_address, bool time64) {
  if (count > 16) {
    return error(Errno::invalid_argument);
  }
  constexpr u32 set_size = sizeof(u32);
  const u32 addresses[]{read_address, write_address, error_address};
  for (u32 address : addresses) {
    if (address != 0 && !user_memory.contains(address, set_size)) {
      return error(Errno::bad_address);
    }
  }
  const u32 requested_read =
      read_address == 0 ? 0 : *reinterpret_cast<const u32*>(read_address);
  const u32 requested_write =
      write_address == 0 ? 0 : *reinterpret_cast<const u32*>(write_address);
  const u32 requested_error =
      error_address == 0 ? 0 : *reinterpret_cast<const u32*>(error_address);
  u64 deadline = ~u64{0};
  if (timeout_address != 0) {
    constexpr u64 ticks_per_second = 10'000'000;
    if (time64) {
      if (!user_memory.contains(timeout_address, sizeof(Timespec64))) {
        return error(Errno::bad_address);
      }
      const auto timeout =
          *reinterpret_cast<const Timespec64*>(timeout_address);
      deadline = arch::time_ticks() + timeout.seconds * ticks_per_second +
                 static_cast<u32>(timeout.nanoseconds) / 100;
    } else {
      if (!user_memory.contains(timeout_address, sizeof(Timespec32))) {
        return error(Errno::bad_address);
      }
      const auto timeout =
          *reinterpret_cast<const Timespec32*>(timeout_address);
      if (timeout.seconds < 0 || timeout.nanoseconds < 0) {
        return error(Errno::invalid_argument);
      }
      deadline = arch::time_ticks() +
                 static_cast<u32>(timeout.seconds) * ticks_per_second +
                 static_cast<u32>(timeout.nanoseconds) / 100;
    }
  }
  for (;;) {
    network::poll();
    if (resume_interactive_child_if_ready(frame)) {
      return static_cast<i32>(frame.x[10]);
    }
    // BusyBox and other foreground programs may use pselect() rather than a
    // direct UART read while a background service is parked.  Network
    // readiness must preempt that wait just as it preempts read() and ppoll().
    if (resume_background_if_ready(frame)) {
      return static_cast<i32>(frame.x[10]);
    }
    u32 ready_read = 0;
    u32 ready_write = 0;
    u32 ready_error = 0;
    i32 ready_count = 0;
    for (u32 descriptor = 0; descriptor < count; ++descriptor) {
      const u32 bit = 1u << descriptor;
      bool ready = false;
      if ((requested_read & bit) != 0) {
        if (descriptor_read_ready(descriptor)) {
          ready_read |= bit;
          ready = true;
        }
      }
      if ((requested_write & bit) != 0 &&
          descriptor_write_ready(descriptor)) {
        ready_write |= bit;
        ready = true;
      }
      if ((requested_error & bit) != 0 &&
          descriptor_error_ready(descriptor)) {
        ready_error |= bit;
        ready = true;
      }
      if (ready) {
        ++ready_count;
      }
    }
    if (ready_count != 0 || arch::time_ticks() >= deadline) {
      if (read_address != 0) {
        *reinterpret_cast<u32*>(read_address) = ready_read;
      }
      if (write_address != 0) {
        *reinterpret_cast<u32*>(write_address) = ready_write;
      }
      if (error_address != 0) {
        *reinterpret_cast<u32*>(error_address) = ready_error;
      }
      return ready_count;
    }
    for (u32 descriptor = 0; descriptor < count; ++descriptor) {
      const u32 bit = 1u << descriptor;
      if ((requested_read & bit) == 0) {
        continue;
      }
      const auto* slot = descriptor_slot(descriptor);
#ifdef MIKOS_TRIBE_INTERACTIVE
      static bool reported_pselect_wait = false;
      if (!reported_pselect_wait && slot != nullptr &&
          slot->node == Node::network_socket) {
        reported_pselect_wait = true;
        write_text("MIKOS:PSELECT_NETWORK_WAIT fd=");
        write_u32(descriptor);
        write_text("\n");
      }
#endif
      if (slot != nullptr && slot->node == Node::network_socket &&
          park_background(frame, slot->socket)) {
        return static_cast<i32>(frame.x[10]);
      }
      if (slot != nullptr && slot->node == Node::pty) {
        const auto wait_pty = slot->pty;
        const auto wait_end = slot->pty_end;
        if (park_interactive_child(frame, wait_pty, wait_end)) {
          return static_cast<i32>(frame.x[10]);
        }
      }
    }
  }
}

[[maybe_unused, nodiscard]] i32 chdir(u32 path) {
  char canonical[256]{};
  const Node node = path_node(path, canonical);
  if (node == Node::none) {
    return error(Errno::no_entry);
  }
  if (!directory(node)) {
    return error(Errno::not_directory);
  }
  memcpy(process.current_directory, canonical, sizeof(canonical));
  return 0;
}

[[nodiscard]] i32 execve(TrapFrame& frame, u32 path, u32 argv, u32 envp) {
  char canonical[256]{};
  Node node = path_node(path, canonical);
  if (text_is(canonical, "/proc/self/exe")) {
    u32 index = 0;
    while (index != sizeof(canonical) - 1 &&
           process.executable_path[index] != '\0') {
      canonical[index] = process.executable_path[index];
      ++index;
    }
    canonical[index] = '\0';
    const auto executable = drivers::fs::root::lookup(canonical);
    node = executable ? Node{executable.value} : Node{Node::none};
  }
  if (!user_memory.aligned(argv, alignof(u32)) ||
      (envp != 0 && !user_memory.aligned(envp, alignof(u32)))) {
    return error(Errno::invalid_argument);
  }
  if (node == Node::none || node != Node::filesystem ||
      node.file.type != drivers::fs::root::Type::regular) {
    return error(Errno::no_entry);
  }
  if ((node.file.mode & 0111) == 0) {
    return error(Errno::access_denied);
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  // The interactive flat-address-space scheduler keeps a blocked background
  // service as a memory snapshot. Starting the same executable again would
  // suspend the shell for another full initialization only to fail bind(2)
  // against the original listener. Reject that duplicate before replacing
  // the child image. Once the service exits, background.used is cleared and
  // an intentional restart is allowed.
  if (background.used &&
      text_is(canonical, background.executable_path)) {
    write_text("MIKOS:BACKGROUND_EXEC_BUSY pid=");
    write_u32(background.pid);
    write_text("\n");
    return error(Errno::text_file_busy);
  }
#endif
  constexpr u32 max_arguments = 16;
  constexpr u32 max_argument_size = 128;
  constexpr u32 max_environment = 16;
  constexpr u32 max_environment_size = 128;
  char argument_storage[max_arguments][max_argument_size]{};
  const char* arguments[max_arguments]{};
  char environment_storage[max_environment][max_environment_size]{};
  const char* environment[max_environment]{};
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
  if (count == 0 || count == max_arguments ||
      (parent.active && !save_parent_memory())) {
    return error(Errno::no_memory);
  }
  u32 environment_count = 0;
  if (envp != 0) {
    for (; environment_count < max_environment; ++environment_count) {
      const u32 slot = envp + environment_count * sizeof(u32);
      if (!user_memory.contains(slot, sizeof(u32))) {
        return error(Errno::bad_address);
      }
      const u32 address = *reinterpret_cast<const u32*>(slot);
      if (address == 0) {
        break;
      }
      if (!copy_user_string(address, environment_storage[environment_count],
                            max_environment_size)) {
        return error(Errno::bad_address);
      }
      environment[environment_count] =
          environment_storage[environment_count];
    }
    if (environment_count == max_environment) {
      return error(Errno::no_memory);
    }
  }
#ifdef MIKOS_TRIBE_INTERACTIVE
  static u32 exec_reports = 0;
  if (exec_reports < 12) {
    ++exec_reports;
    write_text("MIKOS:EXEC pid=");
    write_u32(process.pid);
    write_text(" path=");
    write_text(canonical);
    write_text(" argc=");
    write_u32(count);
    for (u32 i = 0; i < count; ++i) {
      write_text(" argv");
      write_u32(i);
      write_text("=");
      write_text(argument_storage[i]);
    }
    write_text("\n");
  }
#endif
  if (!replace_with_executable(frame, canonical, arguments, count,
                               environment, environment_count)) {
    return error(Errno::exec_format);
  }
  return 0;
}

[[nodiscard]] WaitableChild* waitable_for_wait4(u32 pid) {
  const i32 selector = static_cast<i32>(pid);
  for (auto& child : waitable_children) {
    if (!child.used || child.parent_pid != process.pid) {
      continue;
    }
    if (selector == -1 ||
        (selector > 0 && child.pid == static_cast<u32>(selector)) ||
        (selector == 0 && child.process_group == process.process_group) ||
        (selector < -1 &&
         child.process_group == static_cast<u32>(-selector))) {
      return &child;
    }
  }
  return nullptr;
}

[[nodiscard]] i32 wait4(u32 pid, u32 status) {
  auto* child = waitable_for_wait4(pid);
  if (child == nullptr) {
    return error(Errno::no_child);
  }
  if (status != 0) {
    if (!user_memory.contains(status, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u32*>(status) = child->status << 8;
  }
  const u32 result = child->pid;
  *child = {};
  refresh_child_waitable();
  return static_cast<i32>(result);
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

[[nodiscard]] i32 waitid(u32 which, u32 pid, u32 address, u32 options) {
  constexpr u32 all_children = 0;
  constexpr u32 specific_pid = 1;
  constexpr u32 process_group = 2;
  constexpr u32 no_hang = 1;
  constexpr u32 nowait = 0x01000000;
  if (which != all_children && which != specific_pid &&
      which != process_group) {
    return error(Errno::invalid_argument);
  }
  WaitableChild* selected = nullptr;
  for (auto& child : waitable_children) {
    if (child.used && child.parent_pid == process.pid &&
        (which == all_children ||
         (which == specific_pid && child.pid == pid) ||
         (which == process_group &&
          child.process_group ==
              (pid == 0 ? process.process_group : pid)))) {
      selected = &child;
      break;
    }
  }
  if (selected == nullptr) {
    if ((options & no_hang) != 0 &&
        user_memory.contains(address, sizeof(ChildSiginfo))) {
      memset(reinterpret_cast<void*>(address), 0, sizeof(ChildSiginfo));
      return 0;
    }
    return error(Errno::no_child);
  }
  if (!user_memory.contains(address, sizeof(ChildSiginfo))) {
    return error(Errno::bad_address);
  }
  constexpr i32 sigchld = 17;
  constexpr i32 child_exited = 1;
  const ChildSiginfo information{sigchld,
                                 0,
                                 child_exited,
                                 static_cast<i32>(selected->pid),
                                 0,
                                 static_cast<i32>(selected->status),
                                 0,
                                 0,
                                 {}};
  memcpy(reinterpret_cast<void*>(address), &information,
         sizeof(information));
  if ((options & nowait) == 0) {
    *selected = {};
    refresh_child_waitable();
  }
  return 0;
}

struct [[gnu::packed]] SignalAction32 {
  u32 handler;
  u32 flags;
  u64 mask;
};

static_assert(sizeof(SignalAction32) == 16);

// RV32 Linux rt_sigframe is siginfo_t followed by ucontext_t.  RISC-V keeps
// PC in gregs[0], x1..x31 in gregs[1..31], and aligns mcontext to 16 bytes.
struct SignalUcontext32 {
  u32 flags;
  u32 link;
  u32 stack_pointer;
  u32 stack_flags;
  u32 stack_size;
  u8 signal_mask_and_reserved[140];
  u32 general_registers[32];
  u8 floating_point_state[528];
};

struct SignalFrame32 {
  u8 information[128];
  SignalUcontext32 context;
  u32 return_trampoline[2];
  u8 alignment[8];
};

static_assert(sizeof(SignalUcontext32) == 816);
static_assert(__builtin_offsetof(SignalUcontext32, general_registers) == 160);
static_assert(sizeof(SignalFrame32) == 960);

void deliver_pending_signal_impl(TrapFrame& frame) {
  if (active_signal_frame.active) {
    return;
  }
  const u8 signal = signals.next();
  if (signal == 0) {
    return;
  }
  const auto action = signals.action(signal);
  constexpr u32 signal_ignore = 1;
  if (action.handler == signal_ignore ||
      (action.handler == 0 &&
       process_model::SignalState::default_for(signal) ==
           process_model::SignalDefault::ignore)) {
    return;
  }
  if (action.handler == 0) {
    // The current interactive profile only resumes caught/ignored signals.
    // Preserve deterministic fail-closed behavior for an unsupported default
    // terminate/stop action instead of returning to user code silently.
    write_text("MIKOS:UNHANDLED_SIGNAL ");
    write_u32(signal);
    write_text("\n");
    shutdown(128 + signal);
  }

  const u32 old_stack = frame.x[2];
  if (old_stack < sizeof(SignalFrame32)) {
    shutdown(128 + signal);
  }
  const u32 address = align_down(old_stack - sizeof(SignalFrame32),
                                 static_cast<u32>(16));
  if (!user_memory.contains(address, sizeof(SignalFrame32))) {
    shutdown(128 + signal);
  }
  auto* signal_frame = reinterpret_cast<SignalFrame32*>(address);
  memset(signal_frame, 0, sizeof(*signal_frame));
  *reinterpret_cast<i32*>(signal_frame->information) = signal;
  *reinterpret_cast<u64*>(signal_frame->context.signal_mask_and_reserved) =
      signals.blocked();
  signal_frame->context.general_registers[0] = frame.mepc;
  for (u32 index = 1; index < 32; ++index) {
    signal_frame->context.general_registers[index] = frame.x[index];
  }

  active_signal_frame = {frame, signals.blocked(), true};
  // RISC-V Linux normally supplies __kernel_rt_sigreturn from the VDSO.
  // MikOS has no VDSO yet, so execute the equivalent two instructions from
  // the signal frame (the flat user mapping is executable in this profile).
  signal_frame->return_trampoline[0] = 0x08b00893;  // li a7, 139
  signal_frame->return_trampoline[1] = 0x00000073;  // ecall
  constexpr u32 signal_action_siginfo = 4;
  constexpr u32 signal_action_nodefer = 0x40000000;
  u64 handler_mask = signals.blocked() | action.mask;
  if ((action.flags & signal_action_nodefer) == 0) {
    handler_mask |= process_model::SignalState::bit(signal);
  }
  static_cast<void>(signals.change_mask(
      process_model::SignalMaskOperation::set, handler_mask));
  frame.x[1] = address + __builtin_offsetof(SignalFrame32, return_trampoline);
  frame.x[2] = address;
  frame.x[10] = signal;
  if ((action.flags & signal_action_siginfo) != 0) {
    frame.x[11] = address;
    frame.x[12] = address + __builtin_offsetof(SignalFrame32, context);
  }
  frame.mepc = action.handler;
#ifdef MIKOS_TRIBE_INTERACTIVE
  write_text("MIKOS:SIGNAL_DELIVER ");
  write_u32(signal);
  write_text("\n");
#endif
}

[[nodiscard]] i32 rt_sigreturn(TrapFrame& frame) {
  if (!active_signal_frame.active ||
      !user_memory.contains(frame.x[2], sizeof(SignalFrame32))) {
    return error(Errno::bad_address);
  }
  const auto* signal_frame =
      reinterpret_cast<const SignalFrame32*>(frame.x[2]);
  const auto& registers = signal_frame->context.general_registers;
  TrapFrame restored = active_signal_frame.frame;
  restored.mepc = registers[0];
  for (u32 index = 1; index < 32; ++index) {
    restored.x[index] = registers[index];
  }
  const u64 restored_mask =
      *reinterpret_cast<const u64*>(
          signal_frame->context.signal_mask_and_reserved);
  static_cast<void>(signals.change_mask(
      process_model::SignalMaskOperation::set, restored_mask));
  active_signal_frame = {};
  frame = restored;
  process.image_replaced = true;
  return static_cast<i32>(frame.x[10]);
}

[[nodiscard]] i32 rt_sigaction(u32 signal, u32 action_address,
                               u32 old_address, u32 set_size) {
  if (set_size != sizeof(u64) || signal > process_model::signal_max) {
    return error(Errno::invalid_argument);
  }
  const auto number = static_cast<u8>(signal);
  if (old_address != 0) {
    if (!user_memory.contains(old_address, sizeof(SignalAction32))) {
      return error(Errno::bad_address);
    }
    const auto old = signals.action(number);
    const SignalAction32 output{old.handler, old.flags, old.mask};
    memcpy(reinterpret_cast<void*>(old_address), &output, sizeof(output));
  }
  if (action_address == 0) {
    return 0;
  }
  if (!user_memory.contains(action_address, sizeof(SignalAction32))) {
    return error(Errno::bad_address);
  }
  SignalAction32 input{};
  memcpy(&input, reinterpret_cast<const void*>(action_address), sizeof(input));
  const process_model::SignalAction action{input.handler, input.flags,
                                            input.mask};
  switch (signals.set_action(number, action)) {
    case process_model::SignalStatus::success:
      return 0;
    case process_model::SignalStatus::uncatchable:
    case process_model::SignalStatus::invalid_signal:
    case process_model::SignalStatus::invalid_operation:
      return error(Errno::invalid_argument);
  }
  return error(Errno::invalid_argument);
}

[[nodiscard]] i32 rt_sigprocmask(u32 operation, u32 set_address,
                                 u32 old_address, u32 set_size) {
  if (set_size != sizeof(u64)) {
    return error(Errno::invalid_argument);
  }
  if (old_address != 0) {
    if (!user_memory.contains(old_address, sizeof(u64))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u64*>(old_address) = signals.blocked();
  }
  if (set_address == 0) {
    return 0;
  }
  if (!user_memory.contains(set_address, sizeof(u64))) {
    return error(Errno::bad_address);
  }
  process_model::SignalMaskOperation selected{};
  switch (operation) {
    case 0:
      selected = process_model::SignalMaskOperation::block;
      break;
    case 1:
      selected = process_model::SignalMaskOperation::unblock;
      break;
    case 2:
      selected = process_model::SignalMaskOperation::set;
      break;
    default:
      return error(Errno::invalid_argument);
  }
  return signals.change_mask(selected,
                             *reinterpret_cast<const u64*>(set_address)) ==
                 process_model::SignalStatus::success
             ? 0
             : error(Errno::invalid_argument);
}

[[nodiscard]] i32 send_signal(u32 pid, u32 signal) {
  if (signal > process_model::signal_max) {
    return error(Errno::invalid_argument);
  }
  const bool current = pid == 0 || pid == process.pid || pid == ~u32{0};
  const bool suspended_parent =
      parent.active && pid == parent.lineage.pid;
  if (!current && !suspended_parent) {
    return error(Errno::no_entry);
  }
  if (signal == 0) {
    return 0;
  }
  auto& destination = suspended_parent ? parent.signals : signals;
  return destination.queue(static_cast<u8>(signal)) ==
                 process_model::SignalStatus::success
             ? 0
             : error(Errno::invalid_argument);
}

[[nodiscard]] i32 setpgid(u32 pid, u32 group) {
  for (auto& child : waitable_children) {
    if (child.used && child.parent_pid == process.pid && child.pid == pid) {
      child.process_group = group == 0 ? pid : group;
      if (pid == parent.child_pid) {
        parent.child_process_group = child.process_group;
      }
      return 0;
    }
  }
  if (pid != 0 && pid != process.pid) {
    return error(Errno::no_entry);
  }
  const u32 selected = group == 0 ? process.pid : group;
  if (process.session == process.pid) {
    return error(Errno::access_denied);
  }
  if (selected != process.pid && selected != process.process_group &&
      !(parent.active && selected == parent.process_group &&
        process.session == parent.session)) {
    return error(Errno::access_denied);
  }
  process.process_group = selected;
  return 0;
}

[[nodiscard]] i32 getpgid(u32 pid, bool session) {
  if (pid == 0 || pid == process.pid) {
    return static_cast<i32>(session ? process.session : process.process_group);
  }
  if (parent.active && pid == parent.lineage.pid) {
    return static_cast<i32>(session ? parent.session : parent.process_group);
  }
  for (const auto& child : waitable_children) {
    if (child.used && child.parent_pid == process.pid && child.pid == pid) {
      return static_cast<i32>(session ? parent.session : child.process_group);
    }
  }
  return error(Errno::no_entry);
}

[[nodiscard]] i32 setsid() {
  if (process.process_group == process.pid) {
    return error(Errno::access_denied);
  }
  process.process_group = process.pid;
  process.session = process.pid;
  return static_cast<i32>(process.pid);
}

struct [[gnu::packed]] Termios32 {
  u32 input_flags;
  u32 output_flags;
  u32 control_flags;
  u32 local_flags;
  u8 line;
  u8 control_character[19];
};

struct [[gnu::packed]] Termios2 {
  u32 input_flags;
  u32 output_flags;
  u32 control_flags;
  u32 local_flags;
  u8 line;
  u8 control_character[19];
  u32 input_speed;
  u32 output_speed;
};

struct [[gnu::packed]] Winsize {
  u16 rows;
  u16 columns;
  u16 horizontal_pixels;
  u16 vertical_pixels;
};

static_assert(sizeof(Termios32) == sizeof(process_model::TermiosState));
static_assert(sizeof(Termios2) == 44);
static_assert(sizeof(Winsize) == sizeof(process_model::WindowSize));

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

[[maybe_unused, nodiscard]] bool user_string_is(u32 address,
                                                const char* expected) {
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
  if (!user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  if ((descriptor == 1 || descriptor == 2) &&
      standard_redirects[descriptor].node == Node::none) {
    uart_write(reinterpret_cast<const char*>(address), size);
    return static_cast<i32>(size);
  }
  Descriptor* selected = descriptor_slot(descriptor);
  if (selected == nullptr) {
    return error(Errno::bad_file_descriptor);
  }
  auto& slot = *selected;
  if (slot.node == Node::standard_output) {
    uart_write(reinterpret_cast<const char*>(address), size);
    return static_cast<i32>(size);
  }
  if (slot.node == Node::network_socket) {
    if (size == 0) {
      return 0;
    }
    const auto result = network::socket_write(
        slot.socket, reinterpret_cast<const u8*>(address), size);
#ifdef MIKOS_TRIBE_INTERACTIVE
    if (result.result == network::SocketResult::success) {
      static u32 tcp_write_calls = 0;
      ++tcp_write_calls;
      if (tcp_write_calls == 1) {
        write_text("MIKOS:TCP_WRITE ");
        write_u32(result.size);
        write_text("\n");
      }
      if (tcp_write_calls <= 8) {
        write_text("MIKOS:TCP_WRITE_PROGRESS calls=");
        write_u32(tcp_write_calls);
        write_text(" size=");
        write_u32(result.size);
        write_text("\n");
      }
    }
#endif
    return result.result == network::SocketResult::success
               ? static_cast<i32>(result.size)
               : socket_error(result.result);
  }
  if (slot.node == Node::pipe) {
    if (slot.pipe_end != process_model::PipeEnd::write) {
      return error(Errno::bad_file_descriptor);
    }
    const auto result = pipes.write(
        slot.pipe, reinterpret_cast<const u8*>(address), size);
    if (result.status == process_model::PipeStatus::success) {
      return static_cast<i32>(result.size);
    }
    if (result.status == process_model::PipeStatus::broken_pipe) {
      return error(Errno::broken_pipe);
    }
    if (result.status == process_model::PipeStatus::would_block) {
      constexpr u32 nonblock = 0x800;
      if ((slot.flags & nonblock) != 0) {
        return error(Errno::try_again);
      }
      // The syscall is retried by the process-scheduler phase once a reader
      // wakes this writer. Do not busy-loop a full pipe in the current phase.
      return error(Errno::try_again);
    }
    return error(Errno::bad_file_descriptor);
  }
  if (slot.node == Node::pty) {
    const auto result = ptys.write(slot.pty, slot.pty_end,
                                   reinterpret_cast<const u8*>(address), size);
    if (slot.pty_end == process_model::PtyEnd::master &&
        result.generated_signals != 0) {
      const u32 foreground_group = ptys.foreground_group(slot.pty);
      for (u8 signal = 1; signal <= process_model::signal_max; ++signal) {
        if ((result.generated_signals &
             process_model::SignalState::bit(signal)) == 0) {
          continue;
        }
        const u32 targets =
            queue_process_group_signal(foreground_group, signal);
#ifdef MIKOS_TRIBE_INTERACTIVE
        write_text("MIKOS:PTY_SIGNAL signal=");
        write_u32(signal);
        write_text(" group=");
        write_u32(foreground_group);
        write_text(" targets=");
        write_u32(targets);
        write_text("\n");
#else
        static_cast<void>(targets);
#endif
      }
    }
#ifdef MIKOS_TRIBE_INTERACTIVE
    static u32 pty_write_reports = 0;
    if (pty_write_reports < 16) {
      ++pty_write_reports;
      write_text("MIKOS:PTY_WRITE pid=");
      write_u32(process.pid);
      write_text(" fd=");
      write_u32(descriptor);
      write_text(" end=");
      write_text(slot.pty_end == process_model::PtyEnd::master ? "master"
                                                               : "slave");
      write_text(" status=");
      write_u32(static_cast<u32>(result.status));
      write_text(" requested=");
      write_u32(size);
      write_text(" size=");
      write_u32(result.size);
      write_text("\n");
    }
#endif
    switch (result.status) {
      case process_model::PtyStatus::success:
        return static_cast<i32>(result.size);
      case process_model::PtyStatus::would_block:
        return error(Errno::try_again);
      case process_model::PtyStatus::end_of_file:
      case process_model::PtyStatus::io_error:
        return error(Errno::io);
      default:
        return error(Errno::bad_file_descriptor);
    }
  }
  if (slot.node == Node::pseudo &&
      slot.node.pseudo_node == PseudoFilesystem::Node::dev_null) {
    return static_cast<i32>(size);
  }
  if (slot.node != Node::filesystem || directory(slot.node) ||
      (slot.flags & 3) == 0) {
    return error(Errno::bad_file_descriptor);
  }
  if ((slot.flags & 0x400) != 0) {
    slot.offset = node_size(slot.node);
  }
  const auto result = drivers::fs::root::write(
      slot.node.file, slot.offset, reinterpret_cast<const u8*>(address), size);
  if (!result) {
    return error(Errno::io);
  }
  slot.offset += result.value;
  return static_cast<i32>(result.value);
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

[[nodiscard]] i32 read(TrapFrame& frame, u32 descriptor, u32 address,
                       u32 size) {
  if (descriptor >= 3 ||
      (descriptor < 3 &&
       standard_redirects[descriptor].node != Node::none)) {
    return read_virtual(frame, descriptor, address, size);
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
    network::poll();
    if (resume_interactive_child_if_ready(frame)) {
      return static_cast<i32>(frame.x[10]);
    }
    if (resume_background_if_ready(frame)) {
      return static_cast<i32>(frame.x[10]);
    }
  }
  auto* output = reinterpret_cast<u8*>(address);
  output[0] = data;
  u32 count = 1;
  while (count < size && drivers::uart::receive(data)) {
    output[count++] = data;
  }
  return static_cast<i32>(count);
}

[[nodiscard]] i32 getcwd(u32 address, u32 size) {
  const char* path = process.current_directory;
  const u32 required = text_size(path) + 1;
  if (size < required || !user_memory.contains(address, size)) {
    return error(Errno::bad_address);
  }
  memcpy(reinterpret_cast<void*>(address), path, required);
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
  constexpr u32 tcgets2 = 0x802c542a;
  constexpr u32 tcsets2 = 0x402c542b;
  constexpr u32 tcsetsw2 = 0x402c542c;
  constexpr u32 tcsetsf2 = 0x402c542d;
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
  if (request == tcgets2) {
    if (!user_memory.contains(address, sizeof(Termios2))) {
      return error(Errno::bad_address);
    }
    constexpr Termios2 terminal{
        0x00000500,  // ICRNL | IXON
        0x00000005,  // OPOST | ONLCR
        0x000000bf,  // B38400 | CS8 | CREAD
        0x0000803b,  // ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN
        0,
        {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0,
         0},
        38400,
        38400};
    memcpy(reinterpret_cast<void*>(address), &terminal, sizeof(terminal));
    return 0;
  }
  if (request == tcsets || request == tcsetsw || request == tcsetsf) {
    return user_memory.contains(address, sizeof(Termios32))
               ? 0
               : error(Errno::bad_address);
  }
  if (request == tcsets2 || request == tcsetsw2 || request == tcsetsf2) {
    return user_memory.contains(address, sizeof(Termios2))
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

[[nodiscard]] i32 pty_ioctl(Descriptor& slot, u32 request, u32 address) {
  constexpr u32 tcgets = 0x5401;
  constexpr u32 tcsets = 0x5402;
  constexpr u32 tcsetsw = 0x5403;
  constexpr u32 tcsetsf = 0x5404;
  constexpr u32 tcgets2 = 0x802c542a;
  constexpr u32 tcsets2 = 0x402c542b;
  constexpr u32 tcsetsw2 = 0x402c542c;
  constexpr u32 tcsetsf2 = 0x402c542d;
  constexpr u32 tiocsctty = 0x540e;
  constexpr u32 tiocgpgrp = 0x540f;
  constexpr u32 tiocspgrp = 0x5410;
  constexpr u32 tiocgwinsz = 0x5413;
  constexpr u32 tiocswinsz = 0x5414;
  constexpr u32 tiocnotty = 0x5422;
  constexpr u32 tiocgptn = 0x80045430;
  constexpr u32 tiocsptlck = 0x40045431;
  if (request == tiocgptn) {
    if (slot.pty_end != process_model::PtyEnd::master ||
        !user_memory.contains(address, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u32*>(address) = slot.pty.slot;
    return 0;
  }
  if (request == tiocsptlck) {
    if (slot.pty_end != process_model::PtyEnd::master ||
        !user_memory.contains(address, sizeof(i32))) {
      return error(Errno::bad_address);
    }
    return ptys.set_locked(slot.pty,
                           *reinterpret_cast<const i32*>(address) != 0) ==
                   process_model::PtyStatus::success
               ? 0
               : error(Errno::bad_file_descriptor);
  }
  if (request == tcgets) {
    const auto* state = ptys.termios(slot.pty);
    if (state == nullptr ||
        !user_memory.contains(address, sizeof(Termios32))) {
      return error(Errno::bad_address);
    }
    memcpy(reinterpret_cast<void*>(address), state, sizeof(Termios32));
    return 0;
  }
  if (request == tcgets2) {
    const auto* state = ptys.termios(slot.pty);
    if (state == nullptr ||
        !user_memory.contains(address, sizeof(Termios2))) {
      return error(Errno::bad_address);
    }
    Termios2 value{};
    memcpy(&value, state, sizeof(Termios32));
    value.input_speed = 38400;
    value.output_speed = 38400;
    memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    return 0;
  }
  if (request == tcsets || request == tcsetsw || request == tcsetsf) {
    if (!user_memory.contains(address, sizeof(Termios32))) {
      return error(Errno::bad_address);
    }
    process_model::TermiosState state{};
    memcpy(&state, reinterpret_cast<const void*>(address), sizeof(state));
    return ptys.set_termios(slot.pty, state) ==
                   process_model::PtyStatus::success
               ? 0
               : error(Errno::bad_file_descriptor);
  }
  if (request == tcsets2 || request == tcsetsw2 || request == tcsetsf2) {
    if (!user_memory.contains(address, sizeof(Termios2))) {
      return error(Errno::bad_address);
    }
    process_model::TermiosState state{};
    memcpy(&state, reinterpret_cast<const void*>(address), sizeof(state));
    return ptys.set_termios(slot.pty, state) ==
                   process_model::PtyStatus::success
               ? 0
               : error(Errno::bad_file_descriptor);
  }
  if (request == tiocgwinsz) {
    const auto* window = ptys.window(slot.pty);
    if (window == nullptr ||
        !user_memory.contains(address, sizeof(Winsize))) {
      return error(Errno::bad_address);
    }
    memcpy(reinterpret_cast<void*>(address), window, sizeof(Winsize));
    return 0;
  }
  if (request == tiocswinsz) {
    if (!user_memory.contains(address, sizeof(Winsize))) {
      return error(Errno::bad_address);
    }
    process_model::WindowSize window{};
    memcpy(&window, reinterpret_cast<const void*>(address), sizeof(window));
    bool changed = false;
    const auto result = ptys.set_window(slot.pty, window, &changed);
    if (changed && ptys.foreground_group(slot.pty) != 0) {
      static_cast<void>(signals.queue(process_model::signal_window_change));
    }
    return result == process_model::PtyStatus::success
               ? 0
               : error(Errno::bad_file_descriptor);
  }
  if (request == tiocsctty) {
    if (slot.pty_end != process_model::PtyEnd::slave) {
      return error(Errno::not_a_tty);
    }
    return ptys.acquire_controlling_terminal(
               slot.pty, process.pid, process.process_group, process.session,
               address != 0) == process_model::PtyStatus::success
               ? 0
               : error(Errno::access_denied);
  }
  if (request == tiocnotty) {
    return ptys.detach(slot.pty, process.session) ==
                   process_model::PtyStatus::success
               ? 0
               : error(Errno::access_denied);
  }
  if (request == tiocgpgrp) {
    if (!user_memory.contains(address, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    *reinterpret_cast<u32*>(address) = ptys.foreground_group(slot.pty);
    return 0;
  }
  if (request == tiocspgrp) {
    if (!user_memory.contains(address, sizeof(u32))) {
      return error(Errno::bad_address);
    }
    return ptys.set_foreground_group(
               slot.pty, process.session,
               *reinterpret_cast<const u32*>(address)) ==
                   process_model::PtyStatus::success
               ? 0
               : error(Errno::access_denied);
  }
  return error(Errno::not_a_tty);
}

[[nodiscard]] i32 network_ioctl(u32 descriptor, u32 request, u32 address) {
  if (descriptor < 3 || descriptor >= 16 ||
      descriptors[descriptor - 3].node == Node::none) {
    return error(Errno::bad_file_descriptor);
  }
  if (descriptors[descriptor - 3].node != Node::network_socket) {
    return error(Errno::not_a_tty);
  }
  if (request == network::siocgifconf) {
    if (!user_memory.contains(address, sizeof(network::Ifconf32))) {
      return error(Errno::bad_address);
    }
    auto* configuration =
        reinterpret_cast<network::Ifconf32*>(address);
    if (configuration->length < 0) {
      return error(Errno::invalid_argument);
    }
    if (configuration->buffer == 0) {
      configuration->length = sizeof(network::Ifreq32);
      return 0;
    }
    if (configuration->length < static_cast<i32>(sizeof(network::Ifreq32))) {
      configuration->length = 0;
      return 0;
    }
    if (!user_memory.contains(configuration->buffer,
                              sizeof(network::Ifreq32))) {
      return error(Errno::bad_address);
    }
    network::Ifreq32 interface_request{};
    network::write_interface_name(interface_request.name);
    if (network::interface_ioctl(network::siocgifaddr, interface_request) !=
        network::InterfaceControlResult::success) {
      return error(Errno::no_device);
    }
    memcpy(reinterpret_cast<void*>(configuration->buffer),
           &interface_request, sizeof(interface_request));
    configuration->length = sizeof(interface_request);
    return 0;
  }
  if (!user_memory.contains(address, sizeof(network::Ifreq32))) {
    return error(Errno::bad_address);
  }
  network::Ifreq32 interface_request{};
  memcpy(&interface_request, reinterpret_cast<const void*>(address),
         sizeof(interface_request));
  switch (network::interface_ioctl(request, interface_request)) {
    case network::InterfaceControlResult::success:
      memcpy(reinterpret_cast<void*>(address), &interface_request,
             sizeof(interface_request));
      return 0;
    case network::InterfaceControlResult::no_device:
      return error(Errno::no_device);
    case network::InterfaceControlResult::invalid_argument:
      return error(Errno::invalid_argument);
    case network::InterfaceControlResult::unsupported:
      return error(Errno::operation_not_supported);
  }
  return error(Errno::operation_not_supported);
}

[[nodiscard]] i32 ioctl(u32 descriptor, u32 request, u32 address) {
#ifdef MIKOS_TRIBE_INTERACTIVE
  if (descriptor <= 2 && standard_redirects[descriptor].node == Node::none) {
    return uart_ioctl(descriptor, request, address);
  }
#endif
  if (auto* slot = descriptor_slot(descriptor);
      slot != nullptr && slot->node == Node::pty) {
    return pty_ioctl(*slot, request, address);
  }
  return network_ioctl(descriptor, request, address);
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

[[nodiscard]] i32 futex(u32 address, u32 operation, u32 expected) {
  if (!user_memory.contains(address, sizeof(u32)) ||
      !user_memory.aligned(address, alignof(u32))) {
    return error(Errno::bad_address);
  }
  constexpr u32 command_mask = 0x7f;
  constexpr u32 wait = 0;
  constexpr u32 wake = 1;
  constexpr u32 wait_bitset = 9;
  constexpr u32 wake_bitset = 10;
  switch (operation & command_mask) {
    case wait:
    case wait_bitset:
      if (*reinterpret_cast<const u32*>(address) != expected) {
        return error(Errno::try_again);
      }
      // The flat profile has one running thread. A successful spurious wake
      // is permitted by Linux's futex contract and avoids blocking forever
      // when libc probes its synchronization machinery.
      return 0;
    case wake:
    case wake_bitset:
      return 0;
    default:
      return error(Errno::no_syscall);
  }
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

void deliver_pending_signal(TrapFrame& frame) {
  deliver_pending_signal_impl(frame);
}

i32 dispatch_syscall(TrapFrame& frame) {
  static u32 announced_image = ~u32{0};
  if (announced_image != process.image) {
    if (process.image == 0) {
      write_text("MIKOS:BUSYBOX_ENTRY\n");
#ifdef MIKOS_TRIBE_INTERACTIVE
    } else if (text_is(process.executable_path, "/usr/sbin/dropbear")) {
      write_text("MIKOS:DROPBEAR_ENTRY\n");
#endif
    } else {
      write_text("MIKOS:STRESS_NG_ENTRY\n");
    }
    announced_image = process.image;
  }
  const u32 number = frame.x[17];
#if defined(MIKOS_TRIBE_INTERACTIVE) && defined(MIKOS_TRACE_DROPBEAR_SYSCALLS)
  if (text_is(process.executable_path, "/usr/sbin/dropbear")) {
    write_text("MIKOS:DROPBEAR_SYSCALL ");
    write_u32(number);
    write_text(" ");
    write_text(abi::riscv32::name(number));
    write_text("\n");
  }
#endif
#ifdef MIKOS_TRACE_SYSCALLS
  write_text("MIKOS:SYSCALL ");
  write_u32(number);
  write_text("\n");
#endif
  const u32 a0 = frame.x[10];
  const u32 a1 = frame.x[11];
  const u32 a2 = frame.x[12];
  const u32 a3 = frame.x[13];
  const u32 a4 = frame.x[14];
  switch (static_cast<Syscall>(number)) {
    case Syscall::getcwd:
      return getcwd(a0, a1);
    case Syscall::dup3:
      return duplicate_to(a0, a1, a2);
    case Syscall::fcntl:
      return fcntl(a0, a1, a2);
    case Syscall::mkdirat:
      return mkdirat(a0, a1, a2);
    case Syscall::unlinkat:
      return unlinkat(a0, a1, a2);
    case Syscall::renameat:
      return renameat(a0, a1, a2, a3);
    case Syscall::renameat2:
      return a4 == 0 ? renameat(a0, a1, a2, a3)
                     : error(Errno::invalid_argument);
    case Syscall::truncate:
      return truncate_path(a0, a1);
    case Syscall::ftruncate:
      return ftruncate(a0, a1);
    case Syscall::write:
      return write(a0, a1, a2);
    case Syscall::writev:
      return writev(a0, a1, a2);
    case Syscall::pselect6:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return pselect6(frame, a0, a1, a2, a3, a4, false);
#else
      return unknown(number);
#endif
    case Syscall::ppoll:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return ppoll(frame, a0, a1, a2, false);
#else
      return unknown(number);
#endif
    case Syscall::sync:
      return drivers::fs::root::sync() == drivers::fs::Error::none
                 ? 0
                 : error(Errno::io);
    case Syscall::fsync:
    case Syscall::fdatasync:
      if (a0 < 3 || a0 >= 16 ||
          descriptors[a0 - 3].node == Node::none) {
        return error(Errno::bad_file_descriptor);
      }
      return drivers::fs::root::sync() == drivers::fs::Error::none
                 ? 0
                 : error(Errno::io);
    case Syscall::read:
      return read(frame, a0, a1, a2);
    case Syscall::chdir:
      return chdir(a0);
    case Syscall::faccessat:
      return path_node_at(a0, a1) == Node::none ? error(Errno::no_entry) : 0;
    case Syscall::fchmodat:
      return fchmodat(a0, a1, a2);
    case Syscall::openat:
      return openat(a0, a1, a2);
    case Syscall::close:
      return close(a0);
    case Syscall::pipe2:
      return pipe2(a0, a1);
    case Syscall::getdents64:
      return getdents64(a0, a1, a2);
    case Syscall::lseek:
      if (a0 < 3 || a0 >= 16 ||
          descriptors[a0 - 3].node == Node::none) {
        return error(Errno::bad_file_descriptor);
      }
      descriptors[a0 - 3].offset = a1;
      return static_cast<i32>(a1);
    case Syscall::exit:
    case Syscall::exit_group:
      if (parent.active) {
        const u32 restored_child_pid = parent.child_pid;
#ifdef MIKOS_TRIBE_INTERACTIVE
        write_text("MIKOS:CHILD_EXIT pid=");
        write_u32(process.pid);
        write_text(" status=");
        write_u32(a0);
        write_text("\n");
#endif
        if (child_tid_address != 0 &&
            user_memory.contains(child_tid_address, sizeof(u32))) {
          *reinterpret_cast<u32*>(child_tid_address) = 0;
        }
        child_tid_address = 0;
        if (background.used && process.pid == background.pid) {
#ifdef MIKOS_TRIBE_INTERACTIVE
          write_text("MIKOS:BACKGROUND_EXIT status=");
          write_u32(a0);
          write_text("\n");
#endif
          background.used = false;
          background.wait = BackgroundWait::none;
        }
        restore_parent(frame, a0);
        reinstate_suspended_ancestor(frame);
        // The trap handler stores the dispatch result in the restored a0.
        // Returning a constant here made every fork after PID 2 appear to its
        // parent as PID 2 even though waitid correctly reported the real PID.
        return static_cast<i32>(restored_child_pid);
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
      return waitid(a0, a1, a2, a3);
    case Syscall::set_tid_address:
      child_tid_address = a0;
      return static_cast<i32>(process.pid);
    case Syscall::futex:
    case Syscall::futex_time64:
      return futex(a0, a1, a2);
    case Syscall::set_robust_list:
      return 0;
    case Syscall::rseq:
      return error(Errno::no_syscall);
    case Syscall::getpid:
    case Syscall::gettid:
      return static_cast<i32>(process.pid);
    case Syscall::getppid:
      return static_cast<i32>(process.parent_pid);
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
      return clone(frame, a0, a1, a4);
    case Syscall::execve:
      return execve(frame, a0, a1, a2);
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
      if (user_string_is(a1, "") && a0 >= 3 && a0 < 16) {
        if (descriptors[a0 - 3].node == Node::none) {
          return error(Errno::bad_file_descriptor);
        }
        return write_statx(descriptors[a0 - 3].node, a4);
      }
      return write_statx(path_node_at(a0, a1), a4);
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
      return ppoll(frame, a0, a1, a2, true);
#else
      return unknown(number);
#endif
    case Syscall::pselect6_time64:
#ifdef MIKOS_TRIBE_INTERACTIVE
      return pselect6(frame, a0, a1, a2, a3, a4, true);
#else
      return unknown(number);
#endif
    case Syscall::gettimeofday:
      return gettimeofday(a0);
    case Syscall::uname:
      return uname(a0);
    case Syscall::umask:
      return umask(a0);
    case Syscall::getcpu:
      return getcpu(a0, a1);
    case Syscall::sysinfo:
      return sysinfo(a0);
    case Syscall::socket:
      return socket(a0, a1, a2);
    case Syscall::bind:
      return bind(a0, a1, a2);
    case Syscall::listen:
      return listen(a0, a1);
    case Syscall::accept:
      return accept(frame, a0, a1, a2, 0);
    case Syscall::accept4:
      return accept(frame, a0, a1, a2, a3);
    case Syscall::getsockname:
      return socket_name(a0, a1, a2, false);
    case Syscall::getpeername:
      return socket_name(a0, a1, a2, true);
    case Syscall::setsockopt:
      return setsockopt(a0, a3, a4);
    case Syscall::shutdown:
      return shutdown_socket(a0, a1);
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
      return ioctl(a0, a1, a2);
    case Syscall::rt_sigaction:
      return rt_sigaction(a0, a1, a2, a3);
    case Syscall::rt_sigprocmask:
      return rt_sigprocmask(a0, a1, a2, a3);
    case Syscall::sigaltstack:
      return 0;
    case Syscall::kill:
      return send_signal(a0, a1);
    case Syscall::tgkill:
      return a0 == process.pid ? send_signal(a1, a2)
                               : error(Errno::no_entry);
    case Syscall::rt_sigreturn:
      return rt_sigreturn(frame);
    case Syscall::setpgid:
      return setpgid(a0, a1);
    case Syscall::getpgid:
      return getpgid(a0, false);
    case Syscall::getsid:
      return getpgid(a0, true);
    case Syscall::setsid:
      return setsid();
    case Syscall::statfs64:
      return unknown(number);
    case Syscall::readlinkat:
      return readlinkat(a0, a1, a2, a3);
    case Syscall::fstatat64: {
      const bool descriptor_path =
          user_string_is(a1, "") && a0 >= 3 && a0 < 16;
      const Node node = descriptor_path ? descriptors[a0 - 3].node
                                        : path_node_at(a0, a1);
      return node == Node::none ? error(Errno::no_entry)
                                : write_stat(node, a2);
    }
    case Syscall::fstat64:
      return fstat64(a0, a1);
  }
  return unknown(number);
}

}  // namespace mikos
