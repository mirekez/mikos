#include "test_support.hpp"

#include <support/test.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::ext4;

void build_tree(ext4::test::Image& image) {
  image.extent_inode(3, 0x81a4, 5, 11, 1);
  image.extent_inode(4, 0x41ed, 1024, 12, 1);
  image.extent_inode(5, 0x81a4, 4, 13, 1);
  image.directory_entry(10, 0, 2, 12, ".", 2);
  image.directory_entry(10, 12, 2, 12, "..", 2);
  image.directory_entry(10, 24, 3, 20, "alpha.txt", 1);
  image.directory_entry(10, 44, 4, 980, "sub", 2);
  image.directory_entry(12, 0, 4, 12, ".", 2);
  image.directory_entry(12, 12, 2, 12, "..", 2);
  image.directory_entry(12, 24, 5, 1000, "beta", 1);
}

void check_paths(mikos::test::Suite& suite) {
  ext4::test::Image image;
  build_tree(image);
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  auto node = mounted.value.lookup_path("/alpha.txt");
  MIKOS_CHECK(suite, node);
  MIKOS_CHECK(suite, node.value.inode == 3);
  node = mounted.value.lookup_path("//sub/beta");
  MIKOS_CHECK(suite, node);
  MIKOS_CHECK(suite, node.value.inode == 5);
  node = mounted.value.lookup_path("sub/./beta");
  MIKOS_CHECK(suite, node);
  MIKOS_CHECK(suite, node.value.inode == 5);
  node = mounted.value.lookup_path("/sub/../alpha.txt");
  MIKOS_CHECK(suite, node);
  MIKOS_CHECK(suite, node.value.inode == 3);

  const auto wrong_case = mounted.value.lookup_path("/ALPHA.TXT");
  MIKOS_CHECK(suite, !wrong_case);
  MIKOS_CHECK(suite, wrong_case.error == Error::not_found);
  const auto through_file =
      mounted.value.lookup_path("/alpha.txt/child");
  MIKOS_CHECK(suite, !through_file);
  MIKOS_CHECK(suite, through_file.error == Error::not_directory);
}

void check_corrupt_record(mikos::test::Suite& suite) {
  ext4::test::Image image;
  image.directory_entry(10, 0, 2, 10, ".", 2);
  auto mounted = Volume<ext4::test::Device>::mount(image.device);
  const auto root = mounted.value.root();
  const Error error =
      mounted.value.for_each(root.value, [](const Entry&) { return true; });
  MIKOS_CHECK(suite, error == Error::corrupt);
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/ext4/directory"};
  check_paths(suite);
  check_corrupt_record(suite);
  return suite.finish();
}
