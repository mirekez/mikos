#pragma once

namespace mikos {

using u8 = __UINT8_TYPE__;
using u16 = __UINT16_TYPE__;
using u32 = __UINT32_TYPE__;
using u64 = __UINT64_TYPE__;
using i32 = __INT32_TYPE__;
using usize = __SIZE_TYPE__;

struct UserRange {
  u32 begin;
  u32 end;

  [[nodiscard]] constexpr bool contains(u32 address, u32 size) const {
    return address >= begin && address <= end && size <= end - address;
  }

  [[nodiscard]] constexpr bool aligned(u32 address, u32 alignment) const {
    return alignment != 0 && (alignment & (alignment - 1)) == 0 &&
           (address & (alignment - 1)) == 0;
  }
};

template <typename T>
[[nodiscard]] constexpr T align_up(T value, T alignment) {
  return static_cast<T>((value + alignment - 1) & ~(alignment - 1));
}

template <typename T>
[[nodiscard]] constexpr T align_down(T value, T alignment) {
  return static_cast<T>(value & ~(alignment - 1));
}

}  // namespace mikos
