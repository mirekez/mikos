#include <array>
#include <mikos/io/scatter_write.hpp>
#include <support/test.hpp>

int main() {
  mikos::test::Suite suite{"kernel/scatter_write"};
  using namespace mikos;
  const std::array vectors{abi::Iovec32{100, 4}, abi::Iovec32{200, 6}};
  unsigned calls = 0;
  auto writer = [&](u32 base, u32 size) -> i32 {
    MIKOS_CHECK(suite, base == (calls == 0 ? 100u : 200u));
    ++calls;
    return static_cast<i32>(size);
  };
  MIKOS_CHECK(suite,
              io::scatter_write(vectors, writer, -22) == 10 && calls == 2);
  calls = 0;
  MIKOS_CHECK(suite, io::scatter_write(
                         vectors,
                         [&](u32, u32) {
                           ++calls;
                           return 2;
                         },
                         -22) == 2 &&
                         calls == 1);
  calls = 0;
  MIKOS_CHECK(suite,
              io::scatter_write(
                  vectors, [&](u32, u32) { return ++calls == 1 ? 4 : -11; },
                  -22) == 4 &&
                  calls == 2);
  MIKOS_CHECK(suite, io::scatter_write(
                         vectors, [](u32, u32) { return -9; }, -22) == -9);
  calls = 0;
  const std::array overflow{abi::Iovec32{100, 0x7fffffffu},
                            abi::Iovec32{200, 1}};
  MIKOS_CHECK(suite,
              io::scatter_write(overflow, writer, -22) == -22 && calls == 0);
  const std::array empty{abi::Iovec32{0, 0}};
  MIKOS_CHECK(suite, io::scatter_write(empty, writer, -22) == 0 && calls == 0);
  MIKOS_CHECK(suite, io::scatter_write({}, writer, -22) == 0 && calls == 0);
  MIKOS_CHECK(suite,
              io::scatter_write(vectors, [](u32, u32) { return 0; }, -22) == 0);
  return suite.finish();
}
