#include "test_support.hpp"

#include <support/test.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::ext4;

void check_inline_root_extent(mikos::test::Suite& suite) {
  ext4::test::Image image;
  image.extent_inode(3, 0x81a4, 1500, 11, 2);
  u8 first[1024]{};
  u8 second[476]{};
  for (u32 index = 0; index < sizeof(first); ++index) {
    first[index] = static_cast<u8>(index);
  }
  for (u32 index = 0; index < sizeof(second); ++index) {
    second[index] = static_cast<u8>(0x80 + (index & 0x3f));
  }
  image.write_block(11, first, sizeof(first));
  image.write_block(12, second, sizeof(second));

  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  const auto inode = mounted.value.read_inode(3);
  MIKOS_CHECK(suite, inode);
  u8 output[80]{};
  const auto read = mounted.value.read(inode.value, 1000, output,
                                       sizeof(output));
  MIKOS_CHECK(suite, read);
  for (u32 index = 0; index < 24; ++index) {
    MIKOS_CHECK(suite, output[index] == first[1000 + index]);
  }
  for (u32 index = 24; index < sizeof(output); ++index) {
    MIKOS_CHECK(suite, output[index] == second[index - 24]);
  }
}

void check_external_and_legacy(mikos::test::Suite& suite) {
  ext4::test::Image image;
  image.external_extent_inode(3, 0x81a4, 16, 20, 21);
  constexpr u8 external[] = "external extent";
  image.write_block(21, external, sizeof(external));
  image.legacy_inode(4, 0x81a4, 13 * 1024, 22, 23, 24);
  constexpr u8 direct[] = "direct";
  constexpr u8 indirect[] = "indirect";
  image.write_block(22, direct, sizeof(direct));
  image.write_block(24, indirect, sizeof(indirect));

  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  auto inode = mounted.value.read_inode(3);
  u8 output[32]{};
  auto read = mounted.value.read(inode.value, 0, output, 15);
  MIKOS_CHECK(suite, read);
  MIKOS_CHECK(suite, output[0] == 'e');
  MIKOS_CHECK(suite, output[14] == 't');

  inode = mounted.value.read_inode(4);
  read = mounted.value.read(inode.value, 12 * 1024, output, 8);
  MIKOS_CHECK(suite, read);
  MIKOS_CHECK(suite, output[0] == 'i');
  MIKOS_CHECK(suite, output[7] == 't');
}

void check_uninitialized_extent(mikos::test::Suite& suite) {
  ext4::test::Image image;
  image.extent_inode(3, 0x81a4, 32, 11, 1, 0, true);
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  const auto inode = mounted.value.read_inode(3);
  u8 output[32];
  for (auto& byte : output) {
    byte = 0xff;
  }
  const auto read =
      mounted.value.read(inode.value, 0, output, sizeof(output));
  MIKOS_CHECK(suite, read);
  for (auto byte : output) {
    MIKOS_CHECK(suite, byte == 0);
  }
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/ext4/inode_extent"};
  check_inline_root_extent(suite);
  check_external_and_legacy(suite);
  check_uninitialized_extent(suite);
  return suite.finish();
}
