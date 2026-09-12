#pragma once

#include <mikos/base.hpp>

namespace mikos::memory {

// Single-owner, bounded physical-memory arena. The caller owns the backing
// buffer; no mmap, global heap, locks or metadata allocation is involved.
// Free blocks coalesce on every release. Allocation is O(number of blocks).
class Arena {
  struct Block {
    usize bytes;
    Block* next;
    void* user;
  };

 public:
  constexpr Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  [[nodiscard]] bool initialize(void* storage, usize bytes) {
    if (head_ != nullptr || storage == nullptr || bytes < sizeof(Block) ||
        reinterpret_cast<usize>(storage) % alignof(Block) != 0 ||
        bytes > ~usize{0} - reinterpret_cast<usize>(storage)) {
      return false;
    }
    // The backing store is a byte array; placement construction starts the
    // lifetime of each metadata block (see the out-of-line helpers).
    head_ = make_block(storage, bytes, nullptr);
    return true;
  }

  [[nodiscard]] void* try_allocate(usize bytes, usize alignment) {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
      return nullptr;
    for (auto* block = head_; block != nullptr; block = block->next) {
      if (block->user != nullptr) continue;
      const auto begin = reinterpret_cast<usize>(block);
      const usize start = begin + sizeof(Block);
      if (start > ~usize{0} - (alignment - 1)) continue;
      const usize user = align_up(start, alignment);
      const usize padding = user - begin;
      if (padding > block->bytes || bytes > block->bytes - padding) continue;
      usize consumed = padding + bytes;
      if (consumed <= ~usize{0} - (alignof(Block) - 1)) {
        const usize rounded = align_up(consumed, usize{alignof(Block)});
        if (rounded <= block->bytes &&
            block->bytes - rounded >= sizeof(Block) + alignof(Block)) {
          block->next = make_block(reinterpret_cast<void*>(begin + rounded),
                                   block->bytes - rounded, block->next);
          block->bytes = rounded;
        }
      }
      block->user = reinterpret_cast<void*>(user);
      used_ += block->bytes;
      if (used_ > high_water_) high_water_ = used_;
      ++allocations_;
      return block->user;
    }
    return nullptr;
  }

  [[nodiscard]] bool release(void* pointer) {
    if (pointer == nullptr) return true;
    Block* previous = nullptr;
    for (auto* block = head_; block != nullptr; block = block->next) {
      if (block->user != pointer) { previous = block; continue; }
      used_ -= block->bytes;
      --allocations_;
      block->user = nullptr;
      if (block->next != nullptr && block->next->user == nullptr) {
        auto* next = block->next;
        block->bytes += next->bytes;
        block->next = next->next;
        next->~Block();
      }
      if (previous != nullptr && previous->user == nullptr) {
        previous->bytes += block->bytes;
        previous->next = block->next;
        block->~Block();
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] usize used() const { return used_; }
  [[nodiscard]] usize high_water() const { return high_water_; }
  [[nodiscard]] usize allocations() const { return allocations_; }

 private:
  static Block* make_block(void*, usize, Block*);
  Block* head_{};
  usize used_{};
  usize high_water_{};
  usize allocations_{};
};

[[noreturn]] void allocation_failure();
[[noreturn]] void allocation_contract_failure();

}  // namespace mikos::memory
