#pragma once

#include <mikos/memory/allocator.hpp>
#include <mikos/container/inplace_vector.hpp>
#include <array>
#include <list>
#include <map>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mikos {
template <typename T> using vector = std::vector<T, memory::KernelAllocator<T>>;
template <typename T> using list = std::list<T, memory::KernelAllocator<T>>;
template <typename K, typename V, typename Compare = std::less<K>>
using map = std::map<K, V, Compare, memory::KernelAllocator<std::pair<const K, V>>>;
template <typename K, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
using unordered_set = std::unordered_set<K, Hash, Equal, memory::KernelAllocator<K>>;

// libc++ vector::reserve requests exactly n elements. The checked ticket makes
// allocation failure recoverable; subsequent insertions must respect capacity.
template <typename T>
[[nodiscard]] bool try_reserve(vector<T>& values, usize n) {
  static_assert(std::is_trivially_copyable_v<T>,
                "checked growth currently supports non-allocating POD values");
  static_assert(!std::is_same_v<T, bool>,
                "checked growth excludes vector<bool>'s packed allocator");
  if (n <= values.capacity()) return true;
  if (n > values.max_size()) return false;
  memory::Resource::Reservation ticket(values.get_allocator().resource(),
                                       n * sizeof(T), alignof(T));
  if (!ticket) return false;
  values.reserve(n);
  return true;
}

// The following node layouts are deliberately tied to our pinned libc++.
// sizeof/alignof are evaluated for the target, never copied from host values.
// Upgrading the pin requires running the allocation-contract tests on RV32.
template <typename T>
[[nodiscard]] bool try_push_back(list<T>& values, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  using Node = std::__list_node<T, void*>;
  memory::Resource::Reservation ticket(values.get_allocator().resource(),
                                       sizeof(Node), alignof(Node));
  if (!ticket) return false;
  values.push_back(value);
  return true;
}

template <typename K, typename V, typename Compare>
[[nodiscard]] bool try_insert(map<K, V, Compare>& values, const K& key,
                              const V& value) {
  static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>);
  if (values.contains(key)) return true;
  using Node = std::__tree_node<std::__value_type<K, V>, void*>;
  memory::Resource::Reservation ticket(values.get_allocator().resource(),
                                       sizeof(Node), alignof(Node));
  if (!ticket) return false;
  values.emplace(key, value);
  return true;
}

template <typename K, typename Hash, typename Equal>
[[nodiscard]] bool try_reserve(unordered_set<K, Hash, Equal>& values, usize n) {
  // A fixed load factor makes the bucket allocation explicit and bounded.
  if (values.max_load_factor() != 1.0f) return false;
  if (n <= values.bucket_count()) return true;
  usize buckets = 2;
  while (buckets < n) {
    if (buckets > ~usize{0} / 2) return false;
    buckets *= 2;
  }
  if (buckets > ~usize{0} / sizeof(void*)) return false;
  memory::Resource::Reservation ticket(values.get_allocator().resource(),
                                       buckets * sizeof(void*), alignof(void*));
  if (!ticket) return false;
  values.rehash(buckets);
  return true;
}

template <typename K, typename Hash, typename Equal>
[[nodiscard]] bool try_insert(unordered_set<K, Hash, Equal>& values, const K& key) {
  static_assert(std::is_trivially_copyable_v<K>);
  if (values.contains(key)) return true;
  if (values.size() == ~usize{0} || !try_reserve(values, values.size() + 1)) return false;
  using Node = std::__hash_node<K, void*>;
  memory::Resource::Reservation ticket(values.get_allocator().resource(),
                                       sizeof(Node), alignof(Node));
  if (!ticket) return false;
  values.insert(key);
  return true;
}
}  // namespace mikos
