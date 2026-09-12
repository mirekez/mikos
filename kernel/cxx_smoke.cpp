#include <mikos/container/standard.hpp>
#include <mikos/memory/boot_array.hpp>

namespace mikos {
// Runs in the actual freestanding kernel too: every required container is
// instantiated and exercised, so --gc-sections cannot hide missing support.
bool kernel_cxx_smoke() {
  struct Defaults { int sentinel{-1}; };
  memory::BootArray<Defaults[3], 4> defaults;
  defaults.initialize();
  for (auto& row : defaults)
    for (auto& element : row) if (element.sentinel != -1) return false;
  alignas(256) unsigned char storage[8192];
  memory::Arena arena;
  if (!arena.initialize(storage, sizeof(storage))) return false;
  memory::Resource resource(arena);
  {
    vector<int> values{memory::KernelAllocator<int>{resource}};
    if (!try_reserve(values, 16)) return false;
    for (int i = 0; i < 16; ++i) values.push_back(i);
    if (try_reserve(values, 1000000) || values.back() != 15) return false;
    list<int> linked{memory::KernelAllocator<int>{resource}};
    if (!try_push_back(linked, 3)) return false;
    linked.push_front(1);
    linked.push_back(2);
    linked.sort();
    if (linked.front() != 1 || linked.back() != 3) return false;
    map<int, int> ordered{memory::KernelAllocator<std::pair<const int, int>>{resource}};
    if (!try_insert(ordered, 9, 90)) return false;
    ordered.emplace(1, 10);
    if (ordered.begin()->first != 1 || ordered.find(9)->second != 90) return false;
    unordered_set<int> unique{memory::KernelAllocator<int>{resource}};
    if (!try_reserve(unique, 16) || !try_insert(unique, 7)) return false;
    unique.insert(7);
    unique.insert(3);
    if (unique.size() != 2 || !unique.contains(7)) return false;
    unique.erase(7);
    if (unique.contains(7)) return false;
    inplace_vector<int, 2> local;
    if (!local.try_push_back(4) || !local.try_push_back(5) ||
        local.try_push_back(6) || local.back() != 5) return false;
  }
  return arena.used() == 0 && arena.allocations() == 0;
}
}  // namespace mikos
