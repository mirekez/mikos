#include "test_support.hpp"

#include <support/test.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::fat32;

void check_lookup_and_read(mikos::test::Suite& suite) {
  fat32::test::Image image;
  constexpr char name[11] = {'P', 'A', 'Y', 'L', 'O', 'A', 'D', ' ',
                             'B', 'I', 'N'};
  image.short_entry(2, 0, name, 0x20, 4, 700);
  image.end_directory(2, 1);
  image.set_fat(4, 8);
  image.set_fat(8, 0x0fffffff);
  u8 first[512]{};
  u8 second[188]{};
  for (u32 index = 0; index < sizeof(first); ++index) {
    first[index] = static_cast<u8>(index * 3);
  }
  for (u32 index = 0; index < sizeof(second); ++index) {
    second[index] = static_cast<u8>(0xf0 - (index & 0x7f));
  }
  image.write_cluster(4, 0, first, sizeof(first));
  image.write_cluster(8, 0, second, sizeof(second));

  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  const auto file = mounted.value.lookup_path("/payload.bin");
  MIKOS_CHECK(suite, file);
  u8 output[256]{};
  const auto read = mounted.value.read(file.value, 500, output,
                                       sizeof(output));
  MIKOS_CHECK(suite, read);
  MIKOS_CHECK(suite, read.value == 200);
  for (u32 index = 0; index < 12; ++index) {
    MIKOS_CHECK(suite, output[index] == first[500 + index]);
  }
  for (u32 index = 12; index < 200; ++index) {
    MIKOS_CHECK(suite, output[index] == second[index - 12]);
  }
  const auto eof = mounted.value.read(file.value, 700, output, 1);
  MIKOS_CHECK(suite, eof);
  MIKOS_CHECK(suite, eof.value == 0);

  const auto directory_read =
      mounted.value.read(mounted.value.root(), 0, output, 1);
  MIKOS_CHECK(suite, !directory_read);
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/fat32/file"};
  check_lookup_and_read(suite);
  return suite.finish();
}
