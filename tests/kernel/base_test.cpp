#include <format>
#include <string>

#include <mikos/base.hpp>

#include <support/test.hpp>

int main() {
  mikos::test::Suite suite{"kernel/base"};
  constexpr mikos::UserRange user{0x81000000, 0x84000000};

  MIKOS_CHECK(suite, user.contains(0x81000000, 1));
  MIKOS_CHECK(suite, user.contains(0x83ffffff, 1));
  MIKOS_CHECK(suite, !user.contains(0x84000000, 1));
  MIKOS_CHECK(suite, !user.contains(0xfffffff0, 0x40));
  MIKOS_CHECK(suite, user.aligned(0x81000000, 16));
  MIKOS_CHECK(suite, !user.aligned(0x81000002, 4));
  MIKOS_CHECK(suite, !user.aligned(0x81000000, 0));
  MIKOS_CHECK(suite, mikos::align_up(17u, 16u) == 32);
  MIKOS_CHECK(suite, mikos::align_down(31u, 16u) == 16);
  MIKOS_CHECK(suite, std::format("{} {}", "C++", 26) == "C++ 26");

  return suite.finish();
}
