#include <mikos/container/standard.hpp>
#include <mikos/memory/boot_array.hpp>
#include <mikos/kernel.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <bit>

extern "C" float ceilf(float) noexcept;

namespace mikos {
bool kernel_cxx_smoke();
void write_text(const char* text) { fputs(text, stderr); }
[[noreturn]] void shutdown(u32 code) { _Exit(static_cast<int>(code)); }
}

namespace {
int failures;
#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); ++failures; \
} } while (false)

struct Object {
  static inline int live;
  int value;
  explicit Object(int v) noexcept : value(v) { ++live; }
  Object(const Object& other) noexcept : value(other.value) { ++live; }
  Object(Object&& other) noexcept : value(other.value) { ++live; }
  Object& operator=(const Object&) = default;
  Object& operator=(Object&&) = default;
  ~Object() { --live; }
};
struct alignas(256) Aligned { int value; };
struct Collision { size_t operator()(int) const noexcept { return 0; } };

void allocator_tests() {
  alignas(256) unsigned char storage[32768];
  mikos::memory::Arena arena;
  CHECK(!arena.initialize(storage + 1, sizeof(storage) - 1));
  CHECK(arena.initialize(storage, sizeof(storage)));
  CHECK(!arena.initialize(storage, sizeof(storage)));
  CHECK(arena.try_allocate(0, 16) == nullptr);
  CHECK(arena.try_allocate(10, 3) == nullptr);
  CHECK(arena.try_allocate(~size_t{0}, 16) == nullptr);
  void* a = arena.try_allocate(100, 256);
  void* b = arena.try_allocate(200, 16);
  void* c = arena.try_allocate(100, 64);
  CHECK(a && b && c);
  CHECK(reinterpret_cast<size_t>(a) % 256 == 0);
  CHECK(!arena.release(storage + 1));
  CHECK(arena.release(b));
  CHECK(!arena.release(b));
  CHECK(arena.release(a));
  CHECK(arena.release(c));
  CHECK(arena.used() == 0);
  void* large = arena.try_allocate(32000, 16);
  CHECK(large != nullptr);
  CHECK(arena.release(large));
  // Fragmentation, non-LIFO frees, alignment, and retained payload bytes.
  struct Allocation { unsigned char* p{}; size_t size{}; unsigned char tag{}; } slots[64];
  unsigned random = 123;
  for (unsigned step = 0; step < 6000; ++step) {
    random = random * 1664525u + 1013904223u;
    auto& slot = slots[(random >> 16) % 64];
    if (slot.p) {
      for (size_t i = 0; i < slot.size; ++i) CHECK(slot.p[i] == slot.tag);
      CHECK(arena.release(slot.p));
      slot = {};
    } else {
      const size_t size = 1 + ((random >> 8) % 512);
      const size_t alignment = size_t{1} << (random % 9);
      auto* p = static_cast<unsigned char*>(arena.try_allocate(size, alignment));
      if (p) {
        CHECK(reinterpret_cast<size_t>(p) % alignment == 0);
        slot = {p, size, static_cast<unsigned char>(step)};
        for (size_t i = 0; i < size; ++i) p[i] = slot.tag;
      }
    }
  }
  for (auto& slot : slots) if (slot.p) CHECK(arena.release(slot.p));
  CHECK(arena.used() == 0 && arena.allocations() == 0);
}

void container_tests() {
  alignas(256) unsigned char storage[32768];
  mikos::memory::Arena arena;
  CHECK(arena.initialize(storage, sizeof(storage)));
  mikos::memory::Resource resource(arena);
  {
    mikos::vector<int> v{mikos::memory::KernelAllocator<int>{resource}};
    CHECK(mikos::try_reserve(v, 64));
    auto* initial = v.data();
    for (int i = 0; i < 64; ++i) v.push_back(i);
    CHECK(v.data() == initial);
    CHECK(!mikos::try_reserve(v, ~size_t{0}));
    CHECK(!mikos::try_reserve(v, 1000000));
    CHECK(v.size() == 64 && v[63] == 63 && v.data() == initial);
    CHECK(mikos::try_reserve(v, 128));
    v.erase(v.begin() + 5);
    CHECK(v[5] == 6);
    auto copy = v;
    auto moved = std::move(copy);
    CHECK(moved.size() == 63 && copy.empty());
    mikos::vector<Aligned> aligned{mikos::memory::KernelAllocator<Aligned>{resource}};
    CHECK(mikos::try_reserve(aligned, 2));
    aligned.push_back({42});
    CHECK(reinterpret_cast<size_t>(aligned.data()) % 256 == 0);

    mikos::map<int,int> ordered{mikos::memory::KernelAllocator<std::pair<const int,int>>{resource}};
    for (int i = 63; i >= 0; --i) CHECK(mikos::try_insert(ordered, i, i * 2));
    CHECK(mikos::try_insert(ordered, 3, 99));
    CHECK(ordered.at(3) == 6 && ordered.size() == 64);
    int index = 0;
    for (const auto& [key, value] : ordered) CHECK(key == index++ && value == key * 2);
    for (int i = 0; i < 64; i += 2) CHECK(ordered.erase(i) == 1);
    CHECK(ordered.size() == 32 && ordered.begin()->first == 1);

    mikos::unordered_set<int, Collision> set{mikos::memory::KernelAllocator<int>{resource}};
    CHECK(mikos::try_reserve(set, 64));
    for (int i = 0; i < 64; ++i) CHECK(mikos::try_insert(set, i));
    CHECK(mikos::try_insert(set, 9) && set.size() == 64);
    for (int i = 0; i < 64; ++i) CHECK(set.contains(i));
    CHECK(set.erase(31) == 1 && !set.contains(31));
    CHECK(mikos::try_insert(set, 31));
    CHECK(mikos::try_reserve(set, 128));
    CHECK(set.contains(63));
    set.max_load_factor(0.5f);
    CHECK(!mikos::try_reserve(set, 256));

    mikos::list<int> first{mikos::memory::KernelAllocator<int>{resource}};
    mikos::list<int> second{mikos::memory::KernelAllocator<int>{resource}};
    CHECK(mikos::try_push_back(first, 3) && mikos::try_push_back(first, 1));
    CHECK(mikos::try_push_back(second, 2));
    const auto allocations = arena.allocations();
    first.splice(first.end(), second);
    CHECK(second.empty() && arena.allocations() == allocations);
    first.sort();
    CHECK(first.front() == 1 && first.back() == 3);
    first.reverse();
    CHECK(first.front() == 3);
    auto list_copy = first;
    CHECK(list_copy.size() == 3);
    {
      mikos::vector<Object> objects{mikos::memory::KernelAllocator<Object>{resource}};
      objects.reserve(4);
      objects.emplace_back(1);
      objects.emplace_back(2);
      auto objects_copy = objects;
      CHECK(Object::live == 4);
      objects_copy.clear();
      CHECK(Object::live == 2);
    }
    CHECK(Object::live == 0);
    mikos::inplace_vector<Object, 2> local;
    CHECK(local.try_emplace_back(1) != nullptr);
    CHECK(local.try_emplace_back(2) != nullptr);
    CHECK(local.try_emplace_back(3) == nullptr);
    CHECK(Object::live == 2);
    auto local_copy = local;
    CHECK(Object::live == 4);
    local_copy.clear();
    local.pop_back();
    CHECK(Object::live == 1);
    mikos::inplace_vector<int, 0> empty;
    CHECK(empty.try_push_back(1) == nullptr && empty.empty());
  }
  CHECK(Object::live == 0);
  CHECK(arena.allocations() == 0 && arena.used() == 0);
}

void exhaustion_tests() {
  alignas(64) unsigned char storage[1024];
  mikos::memory::Arena arena;
  CHECK(arena.initialize(storage, sizeof(storage)));
  mikos::memory::Resource resource(arena);
  {
    mikos::list<int> list{mikos::memory::KernelAllocator<int>{resource}};
    int count = 0;
    while (mikos::try_push_back(list, count)) ++count;
    CHECK(count > 0 && list.size() == static_cast<size_t>(count));
    list.pop_front();
    CHECK(mikos::try_push_back(list, 42));
  }
  {
    mikos::map<int,int> map{mikos::memory::KernelAllocator<std::pair<const int,int>>{resource}};
    int count = 0;
    while (mikos::try_insert(map, count, count)) ++count;
    CHECK(count > 0 && map.size() == static_cast<size_t>(count));
    CHECK(mikos::try_insert(map, 0, 999));
  }
  {
    mikos::unordered_set<int> set{mikos::memory::KernelAllocator<int>{resource}};
    int count = 0;
    while (mikos::try_insert(set, count)) ++count;
    CHECK(count > 0 && set.size() == static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) CHECK(set.contains(i));
  }
  {
    mikos::memory::Resource::Reservation unused(resource, 128, 16);
    CHECK(static_cast<bool>(unused));
  }
  CHECK(arena.used() == 0);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    (void)resource.allocate(1000000, 16);
    _Exit(99);
  }
  if (child > 0) {
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 30);
  }
  struct Defaults { int sentinel{-1}; };
  mikos::memory::BootArray<Defaults[3], 32> table;
  table.initialize();
  for (auto& row : table) for (auto& item : row) CHECK(item.sentinel == -1);
}
}

void propagation_and_math_tests() {
  alignas(64) unsigned char left_storage[2048], right_storage[2048];
  mikos::memory::Arena left, right;
  CHECK(left.initialize(left_storage, sizeof(left_storage)));
  CHECK(right.initialize(right_storage, sizeof(right_storage)));
  mikos::memory::Resource lr(left), rr(right);
  {
    mikos::memory::KernelAllocator<int> la(lr), ra(rr);
    CHECK(la != ra && la == mikos::memory::KernelAllocator<long>(la));
    mikos::vector<int> a{la}, b{ra};
    CHECK(mikos::try_reserve(a, 8) && mikos::try_reserve(b, 8));
    a.push_back(11);
    b.push_back(22);
    b = a;
    CHECK(b.front() == 11 && b.get_allocator() == ra);
    b = std::move(a);
    CHECK(b.get_allocator() == la && a.empty());
    mikos::vector<int> c{ra};
    CHECK(mikos::try_reserve(c, 8));
    c.push_back(33);
    c.swap(b);
    CHECK(c.front() == 11 && c.get_allocator() == la);
    CHECK(b.front() == 33 && b.get_allocator() == ra);
  }
  CHECK(left.used() == 0 && right.used() == 0);
  struct Case { unsigned input; unsigned output; } cases[] = {
    {0, 0}, {0x80000000, 0x80000000}, {0x3f000000, 0x3f800000},
    {0xbf000000, 0x80000000}, {1, 0x3f800000}, {0x80000001, 0x80000000},
    {0x3f800000, 0x3f800000}, {0x3f800001, 0x40000000},
    {0xbfc00000, 0xbf800000}, {0x4b000000, 0x4b000000},
    {0x7f800000, 0x7f800000}, {0xff800000, 0xff800000},
    {0x7fc00001, 0x7fc00001}
  };
  float (*volatile ceiling)(float) = ceilf;
  for (const auto& test : cases)
    CHECK(std::bit_cast<unsigned>(ceiling(std::bit_cast<float>(test.input))) == test.output);
}

int main() {
  CHECK(mikos::kernel_cxx_smoke());
  allocator_tests();
  container_tests();
  exhaustion_tests();
  propagation_and_math_tests();
  if (failures == 0) puts("PASS: kernel standard containers, arena, exhaustion, lifetimes and boot initialization");
  return failures != 0;
}
