#pragma once

#include <mikos/base.hpp>

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
struct [[nodiscard]] Result {
  T value{};
  Error error{Error::none};

  [[nodiscard]] constexpr operator bool() const {
    return error == Error::none;
  }

  [[nodiscard]] static constexpr Result success(T value) {
    return Result{value, Error::none};
  }

  [[nodiscard]] static constexpr Result failure(Error error) {
    return Result{{}, error};
  }
};

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

  [[nodiscard]] constexpr bool equals(const char* other,
                                      bool ascii_fold = false) const {
    if (other == nullptr) {
      return false;
    }
    u32 index = 0;
    while (index < size && other[index] != '\0') {
      char left = data[index];
      char right = other[index];
      if (ascii_fold) {
        if (left >= 'a' && left <= 'z') {
          left = static_cast<char>(left - ('a' - 'A'));
        }
        if (right >= 'a' && right <= 'z') {
          right = static_cast<char>(right - ('a' - 'A'));
        }
      }
      if (left != right) {
        return false;
      }
      ++index;
    }
    return index == size && other[index] == '\0';
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
  for (u32 index = 0; index < size; ++index) {
    output[index] = 0;
  }
}

}  // namespace mikos::drivers::fs
