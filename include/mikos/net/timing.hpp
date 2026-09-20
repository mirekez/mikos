#pragma once
#include <algorithm>
#include <mikos/base.hpp>

namespace mikos::network {
// CLINT frequency used by the architecture clock and Linux-ABI clock syscalls.
inline constexpr u64 tcp_second = 10'000'000;
inline constexpr u64 tcp_initial_rto = tcp_second;
inline constexpr u64 tcp_max_rto = 60 * tcp_second;
inline constexpr u64 tcp_user_timeout = 120 * tcp_second;
inline constexpr u64 tcp_syn_timeout = 120 * tcp_second;
inline constexpr u64 tcp_fin_wait_timeout = 120 * tcp_second;
inline constexpr u64 tcp_time_wait = 120 * tcp_second;  // 2 * MSL (60 s).
inline constexpr u64 tcp_driver_retry = tcp_second / 100;

// All intervals are < 2^63 ticks; subtraction also works over clock wrap.
[[nodiscard]] constexpr bool deadline_reached(u64 now, u64 deadline) {
  return now - deadline < (u64{1} << 63);
}
[[nodiscard]] constexpr u64 backoff(u64 delay) {
  return std::min(delay * 2, tcp_max_rto);
}
}  // namespace mikos::network
