#pragma once

#include <mikos/base.hpp>
#include <mikos/scheduler.hpp>

namespace mikos {

inline constexpr u32 ram_begin = 0x80000000;
inline constexpr u32 user_begin = 0x81000000;
inline constexpr u32 user_end = 0x82000000;
inline constexpr u32 user_stack_top = 0x81fff000;
inline constexpr UserRange user_memory{user_begin, user_end};

struct Process {
  u32 brk;
  u32 mmap_cursor;
  u32 mutable_begin;
  u32 exit_code;
  u32 image;
  u32 pid;
  bool cwd_proc;
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
void start_stress_ng(TrapFrame& frame);
[[nodiscard]] bool replace_with_busybox(TrapFrame& frame,
                                        const char* const* arguments,
                                        u32 argument_count);

}  // namespace mikos

namespace mikos::network {
[[nodiscard]] bool boot_probe();
}

extern "C" {
void trap_handler(mikos::TrapFrame* frame);
[[noreturn]] void enter_user(mikos::u32 entry, mikos::u32 stack);
extern const mikos::u8 _binary_busybox_busybox_start[];
extern const mikos::u8 _binary_busybox_busybox_end[];
extern const mikos::u8 _binary_stress_ng_source_stress_ng_start[];
extern const mikos::u8 _binary_stress_ng_source_stress_ng_end[];
extern const mikos::u8 uncooperative_probe_begin[];
extern const mikos::u8 uncooperative_probe_end[];
}
