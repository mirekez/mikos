#pragma once

#include <mikos/base.hpp>
#include <algorithm>
#include <expected>
#include <span>
#include <string_view>

namespace mikos::drivers::fs {

enum class Error : u8 {
  none,
  io,
  invalid_argument,
  invalid_format,
  unsupported,
  corrupt,
  out_of_bounds,
  not_found,
  not_directory,
  is_directory,
  loop,
  already_exists,
  no_space,
};

template <typename T>
using Result = std::expected<T, Error>;

template <typename Device>
concept ReadableDevice = requires(Device& device, u64 offset, u8* output,
                                  u32 size) {
  device.read(offset, output, size);
  device.size();
};

template <typename Device>
concept WritableDevice = ReadableDevice<Device> &&
    requires(Device& device, u64 offset, const u8* input, u32 size) {
      { device.write(offset, input, size) };
      { device.flush() };
    };

template <typename Filesystem>
concept MutableFilesystem =
    requires(Filesystem& filesystem, const char* first, const char* second,
             const u8* input, u8* output, u32 size, u64 offset) {
      { filesystem.create(first, input, size) };
      { filesystem.move(first, second) };
      { filesystem.concatenate(first, second) };
      { filesystem.remove(first) };
      { filesystem.read(first, offset, output, size) };
      { filesystem.file_size(first) };
      { filesystem.consistent() };
    };

struct Name {
  static constexpr u32 capacity = 768;

  char data[capacity + 1]{};
  u16 size{};

  [[nodiscard]] constexpr bool empty() const { return size == 0; }

  [[nodiscard]] constexpr std::string_view view() const { return {data, size}; }

  [[nodiscard]] constexpr bool equals(const char* other,
                                      bool ascii_fold = false) const {
    if (other == nullptr) {
      return false;
    }
    // Bound the C-string scan by our own length, as the old comparison did.
    usize length = 0;
    while (length < size && other[length] != '\0') ++length;
    if (length != size || other[length] != '\0') return false;
    const auto fold = [ascii_fold](char c) {
      return ascii_fold && c >= 'a' && c <= 'z' ? char(c - ('a' - 'A')) : c;
    };
    return std::ranges::equal(view(), std::string_view{other, length}, {}, fold, fold);
  }
};

[[nodiscard]] constexpr u16 little_u16(const u8* data) {
  return static_cast<u16>(data[0]) |
         static_cast<u16>(static_cast<u16>(data[1]) << 8);
}

[[nodiscard]] constexpr u32 little_u32(const u8* data) {
  return static_cast<u32>(data[0]) |
         (static_cast<u32>(data[1]) << 8) |
         (static_cast<u32>(data[2]) << 16) |
         (static_cast<u32>(data[3]) << 24);
}

[[nodiscard]] constexpr u64 little_u64(const u8* data) {
  return static_cast<u64>(little_u32(data)) |
         (static_cast<u64>(little_u32(data + 4)) << 32);
}

[[nodiscard]] constexpr bool power_of_two(u32 value) {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] constexpr bool add_overflows(u64 left, u64 right) {
  return right > ~u64{0} - left;
}

[[nodiscard]] constexpr bool multiply_overflows(u64 left, u64 right) {
  return left != 0 && right > ~u64{0} / left;
}

template <ReadableDevice Device>
[[nodiscard]] Error read_exact(Device& device, u64 offset, u8* output,
                               u32 size) {
  if (output == nullptr && size != 0) {
    return Error::invalid_argument;
  }
  const u64 device_size = static_cast<u64>(device.size());
  if (offset > device_size || size > device_size - offset) {
    return Error::out_of_bounds;
  }
  return device.read(offset, output, size) ? Error::none : Error::io;
}

template <WritableDevice Device>
[[nodiscard]] Error write_exact(Device& device, u64 offset, const u8* input,
                                u32 size) {
  if (input == nullptr && size != 0) {
    return Error::invalid_argument;
  }
  const u64 device_size = static_cast<u64>(device.size());
  if (offset > device_size || size > device_size - offset) {
    return Error::out_of_bounds;
  }
  return device.write(offset, input, size) ? Error::none : Error::io;
}

inline void zero_bytes(u8* output, u32 size) {
  std::ranges::fill(std::span{output, size}, u8{});
}

}  // namespace mikos::drivers::fs
