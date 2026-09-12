#pragma once

#include <mikos/abi/iovec.hpp>
#include <span>

namespace mikos::io {
// The caller owns a validated snapshot of ABI descriptors. Writer returns
// actual progress or a negative errno. No allocation or ABI C++ object layout.
template <typename Writer>
[[nodiscard]] i32 scatter_write(std::span<const abi::Iovec32> vectors,
                                Writer&& write, i32 invalid_argument) {
  u32 requested = 0;
  for (const auto& vector : vectors) {
    if (vector.size > 0x7fffffffu - requested) return invalid_argument;
    requested += vector.size;
  }
  u32 total = 0;
  for (const auto& vector : vectors) {
    if (vector.size == 0) continue;
    const i32 result = write(vector.base, vector.size);
    if (result < 0) return total ? static_cast<i32>(total) : result;
    if (static_cast<u32>(result) > vector.size) return invalid_argument;
    total += static_cast<u32>(result);
    if (static_cast<u32>(result) < vector.size) break;
  }
  return static_cast<i32>(total);
}
}  // namespace mikos::io
