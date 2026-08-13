#pragma once

#include <mikos/base.hpp>
#include <mikos/net/interface.hpp>
#include <mikos/net/socket.hpp>
#include <mikos/scheduler.hpp>

namespace mikos {

inline constexpr u32 ram_begin = 0x80000000;
inline constexpr u32 user_begin = 0x81000000;
inline constexpr u32 user_end = 0x82000000;
inline constexpr u32 user_stack_top = 0x81fff000;
inline constexpr UserRange user_memory{user_begin, user_end};

struct Process {
  u32 brk;
  u32 mmap_begin;
  u32 mmap_cursor;
  u32 mutable_begin;
  u32 exit_code;
  u32 image;
  u32 pid;
  u32 parent_pid;
  u32 process_group;
  u32 session;
  char current_directory[256];
  char executable_path[256];
  bool image_replaced;
};

extern Process process;
extern Scheduler scheduler;

void uart_put(char value);
void uart_write(const char* data, u32 size);
void write_text(const char* text);
void write_u32(u32 value);
[[noreturn]] void shutdown(u32 code);
i32 dispatch_syscall(TrapFrame& frame);
void deliver_pending_signal(TrapFrame& frame);
void start_stress_ng(TrapFrame& frame);
void restore_busybox_image();
[[nodiscard]] bool restore_executable_image(const char* path);
[[nodiscard]] bool replace_with_executable(TrapFrame& frame,
                                           const char* path,
                                           const char* const* arguments,
                                           u32 argument_count);

}  // namespace mikos

namespace mikos::network {
[[nodiscard]] bool initialize();
[[nodiscard]] bool boot_probe();
void poll();
[[nodiscard]] InterfaceControlResult interface_ioctl(u32 request,
                                                     Ifreq32& value);
[[nodiscard]] OpenResult socket_open(abi::socket::Type type);
[[nodiscard]] SocketResult socket_retain(u8 handle);
[[nodiscard]] SocketResult socket_close(u8 handle);
[[nodiscard]] SocketResult socket_bind(u8 handle, Endpoint local);
[[nodiscard]] SocketResult socket_listen(u8 handle, u32 backlog);
[[nodiscard]] AcceptResult socket_accept(u8 handle);
[[nodiscard]] ReadResult socket_read(u8 handle, u8* output, u32 size);
[[nodiscard]] ReadResult socket_write(u8 handle, const u8* input, u32 size);
[[nodiscard]] SocketResult socket_shutdown(u8 handle, u32 how);
[[nodiscard]] const SocketSlot* socket_slot(u8 handle);
[[nodiscard]] bool socket_readable(u8 handle);
[[nodiscard]] bool socket_writable(u8 handle);
[[nodiscard]] const char* tcp_table();
}

extern "C" {
void trap_handler(mikos::TrapFrame* frame);
[[noreturn]] void enter_user(mikos::u32 entry, mikos::u32 stack);
extern const mikos::u8 uncooperative_probe_begin[];
extern const mikos::u8 uncooperative_probe_end[];
}
