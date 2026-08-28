#pragma once

#include <mikos/base.hpp>

namespace mikos::process_model {

inline constexpr u8 signal_max = 64;
inline constexpr u8 signal_interrupt = 2;
inline constexpr u8 signal_quit = 3;
inline constexpr u8 signal_kill = 9;
inline constexpr u8 signal_child = 17;
inline constexpr u8 signal_continue = 18;
inline constexpr u8 signal_stop = 19;
inline constexpr u8 signal_tty_stop = 20;
inline constexpr u8 signal_tty_input = 21;
inline constexpr u8 signal_tty_output = 22;
inline constexpr u8 signal_urgent = 23;
inline constexpr u8 signal_window_change = 28;

enum class SignalStatus : u8 {
  success,
  invalid_signal,
  uncatchable,
  invalid_operation,
};

enum class SignalMaskOperation : u8 { block, unblock, set };
enum class SignalDefault : u8 { ignore, terminate, stop, resume };

struct SignalAction {
  u32 handler{};
  u32 flags{};
  u64 mask{};

  [[nodiscard]] constexpr bool operator==(const SignalAction&) const = default;
};

class SignalState {
 public:
  [[nodiscard]] static constexpr bool valid(u8 signal) {
    return signal != 0 && signal <= signal_max;
  }

  [[nodiscard]] static constexpr u64 bit(u8 signal) {
    return valid(signal) ? (u64{1} << (signal - 1)) : 0;
  }

  [[nodiscard]] constexpr SignalStatus set_action(u8 signal,
                                                   SignalAction action,
                                                   SignalAction* old = nullptr) {
    if (!valid(signal)) {
      return SignalStatus::invalid_signal;
    }
    if (old != nullptr) {
      *old = actions_[signal];
    }
    if (signal == signal_kill || signal == signal_stop) {
      return SignalStatus::uncatchable;
    }
    action.mask &= ~uncatchable_mask();
    actions_[signal] = action;
    return SignalStatus::success;
  }

  [[nodiscard]] constexpr SignalAction action(u8 signal) const {
    return valid(signal) ? actions_[signal] : SignalAction{};
  }

  [[nodiscard]] constexpr SignalStatus change_mask(SignalMaskOperation op,
                                                    u64 value,
                                                    u64* old = nullptr) {
    if (old != nullptr) {
      *old = blocked_;
    }
    value &= ~uncatchable_mask();
    switch (op) {
      case SignalMaskOperation::block:
        blocked_ |= value;
        break;
      case SignalMaskOperation::unblock:
        blocked_ &= ~value;
        break;
      case SignalMaskOperation::set:
        blocked_ = value;
        break;
      default:
        return SignalStatus::invalid_operation;
    }
    blocked_ &= ~uncatchable_mask();
    return SignalStatus::success;
  }

  [[nodiscard]] constexpr SignalStatus queue(u8 signal) {
    if (!valid(signal)) {
      return SignalStatus::invalid_signal;
    }
    pending_ |= bit(signal);
    return SignalStatus::success;
  }

  [[nodiscard]] constexpr u8 next() {
    const u64 deliverable = pending_ & ~blocked_;
    for (u8 signal = 1; signal <= signal_max; ++signal) {
      if ((deliverable & bit(signal)) != 0) {
        pending_ &= ~bit(signal);
        return signal;
      }
    }
    return 0;
  }

  [[nodiscard]] constexpr u64 pending() const { return pending_; }
  [[nodiscard]] constexpr u64 blocked() const { return blocked_; }
  [[nodiscard]] constexpr bool has_deliverable() const {
    return (pending_ & ~blocked_) != 0;
  }

  [[nodiscard]] static constexpr SignalDefault default_for(u8 signal) {
    if (signal == signal_child || signal == signal_urgent ||
        signal == signal_window_change) {
      return SignalDefault::ignore;
    }
    if (signal == signal_continue) {
      return SignalDefault::resume;
    }
    if (signal == signal_stop || signal == signal_tty_stop ||
        signal == signal_tty_input || signal == signal_tty_output) {
      return SignalDefault::stop;
    }
    return SignalDefault::terminate;
  }

 private:
  [[nodiscard]] static constexpr u64 uncatchable_mask() {
    return bit(signal_kill) | bit(signal_stop);
  }

  SignalAction actions_[signal_max + 1]{};
  u64 blocked_{};
  u64 pending_{};
};

}  // namespace mikos::process_model
