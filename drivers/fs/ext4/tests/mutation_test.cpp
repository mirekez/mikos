#include "test_support.hpp"

#include <support/test.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::ext4;

void check_block_allocation_persists(mikos::test::Suite& suite) {
  ext4::test::Image image;
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  MIKOS_CHECK(suite, mounted->writable_format());
  const auto allocated = mounted->allocate_block();
  MIKOS_CHECK(suite, allocated);
  MIKOS_CHECK(suite, (*allocated) == 11);
  MIKOS_CHECK(suite, image.device.flush_count() == 2);

  auto remounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  const auto next = remounted->allocate_block();
  MIKOS_CHECK(suite, next);
  MIKOS_CHECK(suite, (*next) == 12);
  MIKOS_CHECK(suite, remounted->geometry().free_blocks ==
                         ext4::test::Image::block_count - 13);
  MIKOS_CHECK(suite,
              remounted->release_block((*allocated)) == Error::none);

  auto final_mount = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, final_mount);
  const auto reused = final_mount->allocate_block();
  MIKOS_CHECK(suite, reused);
  MIKOS_CHECK(suite, (*reused) == (*allocated));
  MIKOS_CHECK(suite,
              final_mount->release_block(3) ==
                  Error::invalid_argument);
}

void check_write_failure_is_reported(mikos::test::Suite& suite) {
  ext4::test::Image image;
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  image.device.fail_writes(true);
  const auto allocated = mounted->allocate_block();
  MIKOS_CHECK(suite, !allocated);
  MIKOS_CHECK(suite, allocated.error() == Error::io);
}

void check_inode_allocate_release_and_reuse(mikos::test::Suite& suite) {
  ext4::test::Image image;
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  const auto first = mounted->allocate_inode();
  MIKOS_CHECK(suite, first);
  MIKOS_CHECK(suite, (*first) == 11);

  auto remounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  const auto second = remounted->allocate_inode();
  MIKOS_CHECK(suite, second);
  MIKOS_CHECK(suite, (*second) == 12);
  MIKOS_CHECK(suite,
              remounted->release_inode((*first)) == Error::none);

  auto final_mount = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, final_mount);
  const auto reused = final_mount->allocate_inode();
  MIKOS_CHECK(suite, reused);
  MIKOS_CHECK(suite, (*reused) == 11);
  MIKOS_CHECK(suite,
              final_mount->release_inode(2) ==
                  Error::invalid_argument);
}

void check_create_write_move_concatenate_remove(
    mikos::test::Suite& suite) {
  ext4::test::Image image;
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  static_assert(MutableFilesystem<Volume<ext4::test::Device>>);
  const u8 first[1500] = {1, 2, 3};
  const u8 second[900] = {4, 5, 6};
  MIKOS_CHECK(suite,
              mounted->create("/alpha.bin", first, sizeof(first)) ==
                  Error::none);
  MIKOS_CHECK(suite,
              mounted->create("/beta.bin", second, sizeof(second)) ==
                  Error::none);
  MIKOS_CHECK(suite,
              mounted->move("/alpha.bin", "/moved.bin") ==
                  Error::none);
  MIKOS_CHECK(suite,
              mounted->concatenate("/moved.bin", "/beta.bin") ==
                  Error::none);

  auto remounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  MIKOS_CHECK(suite, remounted->consistent());
  const auto size = remounted->file_size("/moved.bin");
  MIKOS_CHECK(suite, size);
  MIKOS_CHECK(suite, (*size) == sizeof(first) + sizeof(second));
  u8 output[sizeof(first) + sizeof(second)]{};
  const auto read = remounted->read("/moved.bin", 0, output,
                                        sizeof(output));
  MIKOS_CHECK(suite, read);
  MIKOS_CHECK(suite, (*read) == sizeof(output));
  for (u32 index = 0; index < sizeof(first); ++index) {
    MIKOS_CHECK(suite, output[index] == first[index]);
  }
  for (u32 index = 0; index < sizeof(second); ++index) {
    MIKOS_CHECK(suite, output[sizeof(first) + index] == second[index]);
  }
  MIKOS_CHECK(suite,
              remounted->remove("/beta.bin") == Error::none);
  auto final_mount = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, final_mount);
  MIKOS_CHECK(suite,
              final_mount->lookup_path("/beta.bin").error() ==
                  Error::not_found);
}

void check_create_directories(mikos::test::Suite& suite) {
  ext4::test::Image image;
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  MIKOS_CHECK(suite,
              mounted->mkdir("/tmp", 01777) == Error::none);
  MIKOS_CHECK(suite,
              mounted->mkdir("/tmp", 0755) ==
                  Error::already_exists);
  MIKOS_CHECK(suite,
              mounted->mkdir("/tmp/nested", 0750) == Error::none);

  auto remounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  const auto root = remounted->root();
  const auto temporary = remounted->lookup_path("/tmp");
  const auto nested = remounted->lookup_path("/tmp/nested");
  MIKOS_CHECK(suite, root && root->links == 3);
  MIKOS_CHECK(suite, temporary && temporary->directory());
  MIKOS_CHECK(suite, temporary->mode == 041777);
  MIKOS_CHECK(suite, temporary->links == 3);
  MIKOS_CHECK(suite, temporary->size == ext4::test::Image::block_size);
  MIKOS_CHECK(suite, nested && nested->directory());
  MIKOS_CHECK(suite, nested->mode == 040750);
  const auto dot = remounted->lookup((*temporary), ".");
  const auto dot_dot = remounted->lookup((*temporary), "..");
  MIKOS_CHECK(suite, dot && dot->inode == temporary->inode);
  MIKOS_CHECK(suite, dot_dot && dot_dot->inode == root->inode);

  u8 descriptor[32]{};
  MIKOS_CHECK(suite,
              image.device.read(2 * ext4::test::Image::block_size,
                                descriptor, sizeof(descriptor)));
  MIKOS_CHECK(suite, little_u16(descriptor + 0x10) == 3);
  MIKOS_CHECK(suite, remounted->consistent());
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/ext4/mutation"};
  check_block_allocation_persists(suite);
  check_write_failure_is_reported(suite);
  check_inode_allocate_release_and_reuse(suite);
  check_create_write_move_concatenate_remove(suite);
  check_create_directories(suite);
  return suite.finish();
}
