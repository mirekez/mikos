#pragma once

#include <mikos/base.hpp>

namespace mikos {

struct TrapFrame {
  u32 x[32];
  u32 mepc;
  u32 mstatus;
  u32 mcause;
  u32 mtval;
};

static_assert(sizeof(TrapFrame) == 36 * sizeof(u32));

struct Scheduler {
  u32 next_entry{};
  u32 next_stack{};
  u32 timer_traps{};
  u32 user_preemptions{};
  u32 contract_violations{};
  bool start_pending{};

  constexpr void start_next_on_timer(u32 entry, u32 stack) {
    next_entry = entry;
    next_stack = stack;
    start_pending = true;
  }

  constexpr void on_timer(TrapFrame& frame, bool kernel_mie_clear) {
    ++timer_traps;
    constexpr u32 mstatus_mpp = 3u << 11;
    if (!kernel_mie_clear || (frame.mstatus & mstatus_mpp) != 0) {
      ++contract_violations;
      return;
    }

    ++user_preemptions;
    if (!start_pending) {
      return;
    }

    for (auto& value : frame.x) {
      value = 0;
    }
    frame.x[2] = next_stack;
    frame.mepc = next_entry;
    start_pending = false;
  }
};

}  // namespace mikos
