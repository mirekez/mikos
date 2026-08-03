#include "test_support.hpp"

#include <support/test.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::ext4;

void check_valid(mikos::test::Suite& suite) {
  ext4::test::Image image;
  const auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  if (!mounted) {
    return;
  }
  const auto& geometry = mounted.value.geometry();
  MIKOS_CHECK(suite, geometry.block_size == 1024);
  MIKOS_CHECK(suite, geometry.block_count == 64);
  MIKOS_CHECK(suite, geometry.group_count == 1);
  MIKOS_CHECK(suite, geometry.inode_size == 128);
  MIKOS_CHECK(suite, geometry.descriptor_size == 32);
  MIKOS_CHECK(suite, geometry.integrity_verified);
}

void check_block_and_descriptor_sizes(mikos::test::Suite& suite) {
  for (u32 log_size = 1; log_size <= 2; ++log_size) {
    ext4::test::Image image;
    const u32 block_size = 1024u << log_size;
    image.device.write32(1024 + 0x14, 0);
    image.device.write32(1024 + 0x18, log_size);
    image.device.resize(static_cast<u64>(
                            ext4::test::Image::block_count) *
                        block_size);
    const auto mounted =
        Volume<ext4::test::Device>::mount(image.device);
    MIKOS_CHECK(suite, mounted);
    if (mounted) {
      MIKOS_CHECK(suite,
                  mounted.value.geometry().block_size == block_size);
    }
  }

  ext4::test::Image image;
  image.device.write32(1024 + 0x60, 0x42 | 0x80);
  image.device.write16(1024 + 0xfe, 64);
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  if (mounted) {
    MIKOS_CHECK(suite, mounted.value.geometry().descriptor_size == 64);
  }

  image.device.write32(1024 + 0x64, 0x400);
  mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  if (mounted) {
    MIKOS_CHECK(suite,
                mounted.value.geometry().metadata_checksums);
    MIKOS_CHECK(suite,
                !mounted.value.geometry().integrity_verified);
  }
}

void check_feature_rejection(mikos::test::Suite& suite) {
  ext4::test::Image image;
  image.device.write32(1024 + 0x60, 0x42 | 0x10);
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, !mounted);
  MIKOS_CHECK(suite, mounted.error == Error::unsupported);

  image.device.write32(1024 + 0x60, 0x42);
  image.device.write32(1024 + 0x64, 0x200);
  mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, !mounted);
  MIKOS_CHECK(suite, mounted.error == Error::unsupported);
}

void check_bounds_and_io(mikos::test::Suite& suite) {
  ext4::test::Image image;
  image.device.resize(4096);
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, !mounted);
  MIKOS_CHECK(suite, mounted.error == Error::out_of_bounds);

  ext4::test::Image failed;
  failed.device.fail_reads(true);
  mounted = Volume<ext4::test::Device>::mount(failed.device);
  MIKOS_CHECK(suite, !mounted);
  MIKOS_CHECK(suite, mounted.error == Error::io);
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/ext4/superblock"};
  check_valid(suite);
  check_block_and_descriptor_sizes(suite);
  check_feature_rejection(suite);
  check_bounds_and_io(suite);
  return suite.finish();
}
