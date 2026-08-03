#pragma once

#include <mikos/base.hpp>

namespace mikos::io {

template <u32 Capacity>
class RingBuffer {
 public:
  static_assert(Capacity != 0);

  [[nodiscard]] constexpr u32 size() const { return size_; }
  [[nodiscard]] constexpr u32 capacity() const { return Capacity; }
  [[nodiscard]] constexpr u32 free_space() const { return Capacity - size_; }
  [[nodiscard]] constexpr bool empty() const { return size_ == 0; }
  [[nodiscard]] constexpr bool full() const { return size_ == Capacity; }

  constexpr u32 write(const u8* input, u32 count) {
    const u32 amount = count < free_space() ? count : free_space();
    for (u32 i = 0; i < amount; ++i) {
      data_[(head_ + size_ + i) % Capacity] = input[i];
    }
    size_ += amount;
    return amount;
  }

  constexpr u32 read(u8* output, u32 count) {
    const u32 amount = count < size_ ? count : size_;
    for (u32 i = 0; i < amount; ++i) {
      output[i] = data_[(head_ + i) % Capacity];
    }
    head_ = (head_ + amount) % Capacity;
    size_ -= amount;
    return amount;
  }

  constexpr void clear() {
    head_ = 0;
    size_ = 0;
  }

 private:
  u8 data_[Capacity]{};
  u32 head_{};
  u32 size_{};
};

}  // namespace mikos::io
