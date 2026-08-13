#include <mikos/arch.hpp>

namespace mikos::arch {
namespace {

#ifdef MIKOS_TRIBE
inline constexpr u32 mtimecmp = 0x82004100;
inline constexpr u32 mtime = 0x8200c0f8;
#else
inline constexpr u32 mtimecmp = 0x02004000;
inline constexpr u32 mtime = 0x0200bff8;
#endif
#if defined(MIKOS_TRIBE) && defined(MIKOS_TRIBE_INTERACTIVE)
// The cycle-accurate Tribe model advances far more slowly than wall time, so
// switch to frequent polling only while a background network service is
// parked. A 500-tick interval combined with the former eight-poll handler
// caused a permanent interrupt storm before BusyBox's first syscall.
inline constexpr u32 normal_quantum_ticks = 50'000;
inline constexpr u32 network_quantum_ticks = 5'000;
u32 quantum_ticks = normal_quantum_ticks;
#else
inline constexpr u32 quantum_ticks = 200'000;  // 20 ms at QEMU's 10 MHz.
#endif

struct Counter {
  u32 low;
  u32 high;
};

[[nodiscard]] Counter read_time() {
  const auto* words = reinterpret_cast<volatile u32*>(mtime);
  u32 first_high;
  u32 low;
  u32 second_high;
  do {
    first_high = words[1];
    low = words[0];
    second_high = words[1];
  } while (first_high != second_high);
  return Counter{low, second_high};
}

void write_compare(Counter value) {
  auto* words = reinterpret_cast<volatile u32*>(mtimecmp);
  words[0] = 0xffffffff;
  words[1] = value.high;
  words[0] = value.low;
}

}  // namespace

void rearm_scheduler_timer() {
  auto next = read_time();
  const u32 previous = next.low;
  next.low += quantum_ticks;
  if (next.low < previous) {
    ++next.high;
  }
  write_compare(next);
}

void start_scheduler_timer() {
  rearm_scheduler_timer();
  constexpr u32 machine_timer_enable = 1u << 7;
#ifdef MIKOS_TRIBE_INTERACTIVE
  // Tribe exposes CLINT timer pending as STIP while the CPU is outside
  // M-mode, including MikOS user mode. Enable both forms so the same timer
  // continues across privilege transitions.
  constexpr u32 supervisor_timer_enable = 1u << 5;
  constexpr u32 timer_enable = machine_timer_enable | supervisor_timer_enable;
#else
  constexpr u32 timer_enable = machine_timer_enable;
#endif
  asm volatile("csrs mie, %0" : : "r"(timer_enable));
}

void set_network_timer_waiting(bool waiting) {
#if defined(MIKOS_TRIBE) && defined(MIKOS_TRIBE_INTERACTIVE)
  quantum_ticks = waiting ? network_quantum_ticks : normal_quantum_ticks;
  rearm_scheduler_timer();
#else
  static_cast<void>(waiting);
#endif
}

u64 time_ticks() {
  const auto value = read_time();
  return (static_cast<u64>(value.high) << 32) | value.low;
}

}  // namespace mikos::arch
