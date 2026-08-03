#include "test_support.hpp"

#include <support/test.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::fat32;

void check_fragmented_read(mikos::test::Suite& suite) {
  fat32::test::Image image;
  image.set_fat(5, 9);
  image.set_fat(9, 0x0fffffff);
  u8 first[512]{};
  u8 second[512]{};
  for (u32 index = 0; index < 512; ++index) {
    first[index] = static_cast<u8>(index);
    second[index] = static_cast<u8>(0xa0 + (index & 0x1f));
  }
  image.write_cluster(5, 0, first, sizeof(first));
  image.write_cluster(9, 0, second, sizeof(second));

  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  Node file{5, 900, Type::file, 0x20};
  u8 output[80]{};
  const auto read = mounted.value.read(file, 480, output, sizeof(output));
  MIKOS_CHECK(suite, read);
  MIKOS_CHECK(suite, read.value == sizeof(output));
  for (u32 index = 0; index < 32; ++index) {
    MIKOS_CHECK(suite, output[index] == first[480 + index]);
  }
  for (u32 index = 32; index < sizeof(output); ++index) {
    MIKOS_CHECK(suite, output[index] == second[index - 32]);
  }
}

void check_corrupt_chains(mikos::test::Suite& suite) {
  fat32::test::Image image;
  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);

  Node file{5, 700, Type::file, 0x20};
  u8 output[700]{};
  image.set_fat(5, 0x0ffffff7);
  auto read = mounted.value.read(file, 0, output, sizeof(output));
  MIKOS_CHECK(suite, !read);
  MIKOS_CHECK(suite, read.error == Error::corrupt);

  image.set_fat(5, 6);
  image.set_fat(6, 5);
  file.size = (fat32::test::Image::cluster_count + 2) * 512u;
  read = mounted.value.read(
      file,
      static_cast<u64>(fat32::test::Image::cluster_count + 1) * 512,
      output, 1);
  MIKOS_CHECK(suite, !read);
  MIKOS_CHECK(suite, read.error == Error::loop);
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/fat32/cluster_chain"};
  check_fragmented_read(suite);
  check_corrupt_chains(suite);
  return suite.finish();
}
