#include "test_support.hpp"

#include <support/test.hpp>
#include <string>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs;
using namespace mikos::drivers::fs::fat32;

void check_growth_write_and_truncate(mikos::test::Suite& suite) {
  fat32::test::Image image;
  constexpr char name[11] = {'M', 'U', 'T', 'A', 'B', 'L', 'E', ' ',
                             'B', 'I', 'N'};
  image.short_entry(2, 0, name, 0x20, 4, 3);
  image.end_directory(2, 1);
  image.set_fat(4, 0x0fffffff);
  const u8 prefix[3] = {0x11, 0x22, 0x33};
  image.write_cluster(4, 0, prefix, sizeof(prefix));
  const u8 dirty_slack[4] = {0xaa, 0xbb, 0xcc, 0xdd};
  image.write_cluster(4, 100, dirty_slack, sizeof(dirty_slack));

  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  auto found = mounted->lookup_path("/mutable.bin");
  MIKOS_CHECK(suite, found);
  Node file = (*found);

  const u8 payload[40] = {
      1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
      11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
      21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
      31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
  };
  const auto written = mounted->write(file, 500, payload,
                                            sizeof(payload));
  MIKOS_CHECK(suite, written);
  MIKOS_CHECK(suite, (*written) == sizeof(payload));
  MIKOS_CHECK(suite, file.size == 540);

  auto remounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  found = remounted->lookup_path("/mutable.bin");
  MIKOS_CHECK(suite, found);
  MIKOS_CHECK(suite, found->size == 540);
  u8 output[540]{};
  const auto read = remounted->read((*found), 0, output,
                                         sizeof(output));
  MIKOS_CHECK(suite, read);
  MIKOS_CHECK(suite, (*read) == sizeof(output));
  MIKOS_CHECK(suite, output[0] == 0x11);
  MIKOS_CHECK(suite, output[1] == 0x22);
  MIKOS_CHECK(suite, output[2] == 0x33);
  for (u32 index = 3; index < 500; ++index) {
    MIKOS_CHECK(suite, output[index] == 0);
  }
  for (u32 index = 0; index < sizeof(payload); ++index) {
    MIKOS_CHECK(suite, output[500 + index] == payload[index]);
  }

  file = (*found);
  MIKOS_CHECK(suite, remounted->truncate(file, 100) == Error::none);
  MIKOS_CHECK(suite, file.size == 100);
  MIKOS_CHECK(suite, remounted->truncate(file, 0) == Error::none);
  MIKOS_CHECK(suite, file.size == 0);
  MIKOS_CHECK(suite, file.first_cluster == 0);

  auto final_mount = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, final_mount);
  found = final_mount->lookup_path("/mutable.bin");
  MIKOS_CHECK(suite, found);
  MIKOS_CHECK(suite, found->size == 0);
  MIKOS_CHECK(suite, found->first_cluster == 0);
  MIKOS_CHECK(suite, image.device.flush_count() >= 6);
}

void check_create_lfn_and_remove(mikos::test::Suite& suite) {
  fat32::test::Image image;
  image.end_directory(2, 0);
  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  const u8 payload[7] = {9, 8, 7, 6, 5, 4, 3};
  MIKOS_CHECK(suite,
              mounted->create("/A persistent long file name.bin",
                                   payload, sizeof(payload)) == Error::none);

  auto remounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  const auto found =
      remounted->lookup_path("/A persistent long file name.bin");
  MIKOS_CHECK(suite, found);
  MIKOS_CHECK(suite, found->size == sizeof(payload));
  u8 output[sizeof(payload)]{};
  const auto read = remounted->read((*found), 0, output,
                                         sizeof(output));
  MIKOS_CHECK(suite, read);
  for (u32 index = 0; index < sizeof(payload); ++index) {
    MIKOS_CHECK(suite, output[index] == payload[index]);
  }
  MIKOS_CHECK(suite,
              remounted->remove(
                  "/A persistent long file name.bin") == Error::none);

  auto final_mount = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, final_mount);
  const auto missing =
      final_mount->lookup_path("/A persistent long file name.bin");
  MIKOS_CHECK(suite, !missing);
  MIKOS_CHECK(suite, missing.error() == Error::not_found);
}

void check_move_concatenate_and_reuse(mikos::test::Suite& suite) {
  fat32::test::Image image;
  image.end_directory(2, 0);
  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  static_assert(MutableFilesystem<Volume<fat32::test::Device>>);
  const u8 first[5] = {1, 2, 3, 4, 5};
  const u8 second[4] = {6, 7, 8, 9};
  MIKOS_CHECK(suite,
              mounted->create("/first.bin", first, sizeof(first)) ==
                  Error::none);
  MIKOS_CHECK(suite,
              mounted->create("/second.bin", second,
                                   sizeof(second)) == Error::none);
  MIKOS_CHECK(suite,
              mounted->move("/first.bin", "/moved.bin") ==
                  Error::none);
  MIKOS_CHECK(suite,
              mounted->concatenate("/moved.bin", "/second.bin") ==
                  Error::none);
  MIKOS_CHECK(suite, mounted->consistent());

  auto remounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, remounted);
  u8 output[9]{};
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
              remounted->lookup_path("/first.bin").error() ==
                  Error::not_found);
  MIKOS_CHECK(suite,
              remounted->remove("/second.bin") == Error::none);
  MIKOS_CHECK(suite, remounted->consistent());
}

void check_mutation_failures_are_not_reported_as_success(
    mikos::test::Suite& suite) {
  {
    fat32::test::Image image;
    image.end_directory(2, 0);
    auto mounted = Volume<fat32::test::Device>::mount(image.device);
    MIKOS_CHECK(suite, mounted);
    image.device.fail_writes(true);
    const u8 value = 0x5a;
    MIKOS_CHECK(suite,
                mounted->create("/write-fails.bin", &value, 1) ==
                    Error::io);
    image.device.fail_writes(false);
    auto remounted = Volume<fat32::test::Device>::mount(image.device);
    MIKOS_CHECK(suite, remounted);
    MIKOS_CHECK(suite,
                remounted->lookup_path("/write-fails.bin").error() ==
                    Error::not_found);
  }
  {
    fat32::test::Image image;
    image.end_directory(2, 0);
    auto mounted = Volume<fat32::test::Device>::mount(image.device);
    MIKOS_CHECK(suite, mounted);
    image.device.fail_flushes(true);
    MIKOS_CHECK(suite,
                mounted->create("/flush-fails.bin", nullptr, 0) ==
                    Error::io);
    image.device.fail_flushes(false);
    auto remounted = Volume<fat32::test::Device>::mount(image.device);
    MIKOS_CHECK(suite, remounted);
    MIKOS_CHECK(suite,
                remounted->lookup_path("/flush-fails.bin").error() ==
                    Error::not_found);
  }
}

void check_bounded_name_sequences(mikos::test::Suite& suite) {
  fat32::test::Image image;
  image.end_directory(2, 0);
  auto mounted = Volume<fat32::test::Device>::mount(image.device);
  MIKOS_CHECK(suite, mounted);
  if (!mounted) return;
  // 255 UTF-16 units require all 20 LFN slots plus the short-name slot,
  // crossing the 16-entry sector/cluster boundary of this fixture.
  const std::string longest = "/" + std::string(255, 'a');
  MIKOS_CHECK(suite, mounted->create(longest.c_str(), nullptr, 0) == Error::none);
  MIKOS_CHECK(suite, mounted->lookup_path(longest.c_str()));
  MIKOS_CHECK(suite, mounted->remove(longest.c_str()) == Error::none);
  MIKOS_CHECK(suite, !mounted->lookup_path(longest.c_str()));
  MIKOS_CHECK(suite, mounted->create(longest.c_str(), nullptr, 0) == Error::none);
  const std::string too_long = longest + 'b';
  MIKOS_CHECK(suite, mounted->create(too_long.c_str(), nullptr, 0) == Error::invalid_argument);
  const std::string surrogate = "/" + std::string(253, 'b') + "\xf0\x9f\x98\x80";
  MIKOS_CHECK(suite, mounted->create(surrogate.c_str(), nullptr, 0) == Error::none);
  MIKOS_CHECK(suite, mounted->lookup_path(surrogate.c_str()));
  const std::string overflowing_pair = "/" + std::string(254, 'c') + "\xf0\x9f\x98\x80";
  MIKOS_CHECK(suite, mounted->create(overflowing_pair.c_str(), nullptr, 0) == Error::invalid_argument);
  for (const char* invalid : {"/bad\xf0\x9f", "/bad\xc0\xaf", "/bad\xed\xa0\x80",
                               "/trailing.", "/trailing "}) {
    MIKOS_CHECK(suite, mounted->create(invalid, nullptr, 0) == Error::invalid_argument);
  }
}

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/fs/fat32/mutation"};
  check_growth_write_and_truncate(suite);
  check_create_lfn_and_remove(suite);
  check_move_concatenate_and_reuse(suite);
  check_mutation_failures_are_not_reported_as_success(suite);
  check_bounded_name_sequences(suite);
  return suite.finish();
}
