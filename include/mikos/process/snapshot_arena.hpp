#pragma once

#include <mikos/base.hpp>

namespace mikos::process_model {

struct SnapshotAllocation {
  u32 address{};
  u32 size{};

  [[nodiscard]] constexpr bool valid() const {
    return address != 0 && size != 0;
  }
  [[nodiscard]] constexpr bool operator==(
      const SnapshotAllocation&) const = default;
};

enum class SnapshotArenaStatus : u8 {
  success,
  no_space,
  invalid_argument,
};

template <u32 ExtentCapacity = 32>
class SnapshotArena {
 public:
  static_assert(ExtentCapacity != 0);

  [[nodiscard]] constexpr SnapshotArenaStatus initialize(u32 begin, u32 end) {
    begin = align_up(begin, alignment);
    end = align_down(end, alignment);
    if (begin == 0 || begin >= end) {
      return SnapshotArenaStatus::invalid_argument;
    }
    begin_ = begin;
    end_ = end;
    free_count_ = 1;
    free_[0] = {begin, end - begin};
    for (u32 i = 1; i < ExtentCapacity; ++i) {
      free_[i] = {};
    }
    allocated_ = 0;
    return SnapshotArenaStatus::success;
  }

  [[nodiscard]] constexpr SnapshotAllocation allocate(u32 requested) {
    if (requested == 0 || begin_ == 0 ||
        requested > ~u32{0} - (alignment - 1)) {
      return {};
    }
    const u32 size = align_up(requested, alignment);
    if (size < requested) {
      return {};
    }
    for (u32 i = 0; i < free_count_; ++i) {
      if (free_[i].size < size) {
        continue;
      }
      const SnapshotAllocation result{free_[i].address, size};
      free_[i].address += size;
      free_[i].size -= size;
      if (free_[i].size == 0) {
        erase(i);
      }
      allocated_ += size;
      return result;
    }
    return {};
  }

  [[nodiscard]] constexpr SnapshotArenaStatus release(
      SnapshotAllocation allocation) {
    if (!allocation.valid() || begin_ == 0 ||
        (allocation.address & (alignment - 1)) != 0 ||
        (allocation.size & (alignment - 1)) != 0 ||
        allocation.address < begin_ || allocation.address >= end_ ||
        allocation.size > end_ - allocation.address ||
        allocation.size > allocated_) {
      return SnapshotArenaStatus::invalid_argument;
    }

    u32 position = 0;
    while (position < free_count_ &&
           free_[position].address < allocation.address) {
      ++position;
    }
    if ((position != 0 &&
         free_[position - 1].address + free_[position - 1].size >
             allocation.address) ||
        (position != free_count_ &&
         allocation.address + allocation.size > free_[position].address)) {
      return SnapshotArenaStatus::invalid_argument;
    }

    const bool merge_previous =
        position != 0 &&
        free_[position - 1].address + free_[position - 1].size ==
            allocation.address;
    const bool merge_next =
        position != free_count_ &&
        allocation.address + allocation.size == free_[position].address;
    if (merge_previous) {
      free_[position - 1].size += allocation.size;
      if (merge_next) {
        free_[position - 1].size += free_[position].size;
        erase(position);
      }
    } else if (merge_next) {
      free_[position].address = allocation.address;
      free_[position].size += allocation.size;
    } else {
      if (free_count_ == ExtentCapacity) {
        return SnapshotArenaStatus::no_space;
      }
      for (u32 i = free_count_; i > position; --i) {
        free_[i] = free_[i - 1];
      }
      free_[position] = {allocation.address, allocation.size};
      ++free_count_;
    }
    allocated_ -= allocation.size;
    return SnapshotArenaStatus::success;
  }

  [[nodiscard]] constexpr u32 capacity() const {
    return end_ - begin_;
  }
  [[nodiscard]] constexpr u32 allocated() const { return allocated_; }
  [[nodiscard]] constexpr u32 available() const {
    return capacity() - allocated_;
  }
  [[nodiscard]] constexpr u32 largest_available() const {
    u32 largest = 0;
    for (u32 i = 0; i < free_count_; ++i) {
      if (free_[i].size > largest) {
        largest = free_[i].size;
      }
    }
    return largest;
  }
  [[nodiscard]] constexpr u32 free_extents() const { return free_count_; }

  [[nodiscard]] constexpr bool invariant() const {
    if (begin_ == 0 || begin_ >= end_ || free_count_ > ExtentCapacity ||
        allocated_ > capacity()) {
      return false;
    }
    u32 free_bytes = 0;
    u32 previous_end = begin_;
    for (u32 i = 0; i < free_count_; ++i) {
      if (free_[i].size == 0 || free_[i].address < previous_end ||
          free_[i].address < begin_ || free_[i].address >= end_ ||
          free_[i].size > end_ - free_[i].address) {
        return false;
      }
      previous_end = free_[i].address + free_[i].size;
      free_bytes += free_[i].size;
    }
    return free_bytes == capacity() - allocated_;
  }

 private:
  struct Extent {
    u32 address{};
    u32 size{};
  };

  static constexpr u32 alignment = 16;

  constexpr void erase(u32 position) {
    for (u32 i = position + 1; i < free_count_; ++i) {
      free_[i - 1] = free_[i];
    }
    --free_count_;
    free_[free_count_] = {};
  }

  Extent free_[ExtentCapacity]{};
  u32 begin_{};
  u32 end_{};
  u32 allocated_{};
  u32 free_count_{};
};

}  // namespace mikos::process_model
