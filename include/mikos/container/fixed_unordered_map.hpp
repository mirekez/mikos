#pragma once

#include <mikos/base.hpp>

namespace mikos::container {

// Allocation-free unordered associative container for freestanding profiles.
// Linear probing and tombstones keep lookup independent of insertion order;
// capacity exhaustion is explicit through find_or_emplace's nullptr result.
template <typename Key, typename Value, u32 Capacity, typename Hash>
class FixedUnorderedMap {
 public:
  static_assert(Capacity != 0);

  [[nodiscard]] Value* find(const Key& key) {
    Entry* entry = find_entry(key);
    return entry == nullptr ? nullptr : &entry->value;
  }

  [[nodiscard]] const Value* find(const Key& key) const {
    const Entry* entry = find_entry(key);
    return entry == nullptr ? nullptr : &entry->value;
  }

  [[nodiscard]] Value* find_or_emplace(const Key& key, bool& inserted) {
    const u32 start = Hash{}(key) % Capacity;
    Entry* available = nullptr;
    for (u32 probe = 0; probe < Capacity; ++probe) {
      auto& entry = entries_[(start + probe) % Capacity];
      if (entry.status == Status::occupied && entry.key == key) {
        inserted = false;
        return &entry.value;
      }
      if (entry.status == Status::tombstone && available == nullptr) {
        available = &entry;
      }
      if (entry.status == Status::empty) {
        if (available == nullptr) {
          available = &entry;
        }
        break;
      }
    }
    if (available == nullptr) {
      inserted = false;
      return nullptr;
    }
    available->status = Status::occupied;
    available->key = key;
    available->value = {};
    inserted = true;
    return &available->value;
  }

  [[nodiscard]] bool erase(const Key& key) {
    Entry* entry = find_entry(key);
    if (entry == nullptr) {
      return false;
    }
    entry->status = Status::tombstone;
    entry->value = {};
    return true;
  }

  template <typename Predicate>
  void erase_if(Predicate predicate) {
    for (auto& entry : entries_) {
      if (entry.status == Status::occupied && predicate(entry.key)) {
        entry.status = Status::tombstone;
        entry.value = {};
      }
    }
  }

  [[nodiscard]] u32 size() const {
    u32 result = 0;
    for (const auto& entry : entries_) {
      if (entry.status == Status::occupied) {
        ++result;
      }
    }
    return result;
  }

 private:
  enum class Status : u8 { empty, occupied, tombstone };

  struct Entry {
    Status status{Status::empty};
    Key key{};
    Value value{};
  };

  [[nodiscard]] Entry* find_entry(const Key& key) {
    return const_cast<Entry*>(
        static_cast<const FixedUnorderedMap*>(this)->find_entry(key));
  }

  [[nodiscard]] const Entry* find_entry(const Key& key) const {
    const u32 start = Hash{}(key) % Capacity;
    for (u32 probe = 0; probe < Capacity; ++probe) {
      const auto& entry = entries_[(start + probe) % Capacity];
      if (entry.status == Status::empty) {
        return nullptr;
      }
      if (entry.status == Status::occupied && entry.key == key) {
        return &entry;
      }
    }
    return nullptr;
  }

  Entry entries_[Capacity]{};
};

}  // namespace mikos::container
