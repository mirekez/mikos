#pragma once

#include <mikos/base.hpp>

namespace mikos::arch {

inline constexpr u32 scheduler_timer_cause = 0x80000007;

void start_scheduler_timer();
void rearm_scheduler_timer();
[[nodiscard]] u64 time_ticks();

}  // namespace mikos::arch
