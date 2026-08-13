#include <mikos/container/fixed_unordered_map.hpp>

#include <support/test.hpp>

namespace {

struct Key {
  mikos::u32 value{};

  [[nodiscard]] constexpr bool operator==(const Key&) const = default;
};

struct CollidingHash {
  [[nodiscard]] constexpr mikos::u32 operator()(Key) const { return 1; }
};

using Map = mikos::container::FixedUnorderedMap<Key, mikos::u32, 3,
                                                CollidingHash>;

}  // namespace

int main() {
  mikos::test::Suite suite{"kernel/fixed_unordered_map"};
  Map values;
  bool inserted = false;
  auto* one = values.find_or_emplace({1}, inserted);
  MIKOS_CHECK(suite, inserted && one != nullptr);
  *one = 11;
  auto* two = values.find_or_emplace({2}, inserted);
  MIKOS_CHECK(suite, inserted && two != nullptr);
  *two = 22;
  MIKOS_CHECK(suite, values.size() == 2);
  MIKOS_CHECK(suite, values.find({1}) != nullptr && *values.find({1}) == 11);
  MIKOS_CHECK(suite, values.find({2}) != nullptr && *values.find({2}) == 22);

  auto* existing = values.find_or_emplace({1}, inserted);
  MIKOS_CHECK(suite, !inserted && existing == one && *existing == 11);
  MIKOS_CHECK(suite, values.erase({1}));
  MIKOS_CHECK(suite, values.find({1}) == nullptr);
  auto* tombstone_reuse = values.find_or_emplace({3}, inserted);
  MIKOS_CHECK(suite, inserted && tombstone_reuse != nullptr);
  *tombstone_reuse = 33;
  auto* final_slot = values.find_or_emplace({4}, inserted);
  MIKOS_CHECK(suite, inserted && final_slot != nullptr);
  MIKOS_CHECK(suite, values.find_or_emplace({5}, inserted) == nullptr);

  values.erase_if([](Key key) { return (key.value & 1u) == 0; });
  MIKOS_CHECK(suite, values.find({2}) == nullptr);
  MIKOS_CHECK(suite, values.find({4}) == nullptr);
  MIKOS_CHECK(suite, values.find({3}) != nullptr);
  MIKOS_CHECK(suite, values.size() == 1);

  mikos::u32 visited = 0;
  values.for_each([&](Key key, mikos::u32& value) {
    ++visited;
    MIKOS_CHECK(suite, key.value == 3);
    value = 44;
  });
  MIKOS_CHECK(suite, visited == 1);
  const Map& const_values = values;
  const_values.for_each([&](Key key, const mikos::u32& value) {
    MIKOS_CHECK(suite, key.value == 3);
    MIKOS_CHECK(suite, value == 44);
  });

  return suite.finish();
}
