#include <drivers/storage/sector_reader.hpp>

#include <support/test.hpp>

namespace {

using namespace mikos;
using mikos::drivers::storage::SectorReader;

class Device {
 public:
  static constexpr u32 sector_size = 16;

  Device() {
    for (u32 index = 0; index < sizeof(data_); ++index) {
      data_[index] = static_cast<u8>(index);
    }
  }

  [[nodiscard]] constexpr u64 sector_count() const { return 4; }

  [[nodiscard]] bool read_sector(u64 sector, u8* output) {
    if (sector >= sector_count() || fail_) {
      return false;
    }
    ++reads_;
    for (u32 index = 0; index < sector_size; ++index) {
      output[index] = data_[sector * sector_size + index];
    }
    return true;
  }

  [[nodiscard]] bool write_sector(u64 sector, const u8* input) {
    return write_sectors(sector, input, sector_size);
  }

  [[nodiscard]] bool write_sectors(u64 sector, const u8* input,
                                   u32 byte_count) {
    if (fail_ || input == nullptr || byte_count % sector_size != 0 ||
        sector >= sector_count() ||
        byte_count / sector_size > sector_count() - sector) {
      return false;
    }
    for (u32 index = 0; index < byte_count; ++index) {
      data_[sector * sector_size + index] = input[index];
    }
    return true;
  }

  [[nodiscard]] bool flush() { return !fail_; }

  [[nodiscard]] bool read_sectors(u64 sector, u8* output,
                                  u32 byte_count) {
    if (byte_count % sector_size != 0) {
      return false;
    }
    for (u32 offset = 0; offset < byte_count; offset += sector_size) {
      if (!read_sector(sector + offset / sector_size, output + offset)) {
        return false;
      }
    }
    return true;
  }

  void fail(bool value) { fail_ = value; }
  [[nodiscard]] u32 reads() const { return reads_; }

 private:
  u32 reads_{};
  bool fail_{};
  u8 data_[64]{};
};

}  // namespace

int main() {
  mikos::test::Suite suite{"drivers/sector_reader"};
  Device device;
  SectorReader reader{device};
  u8 output[24]{};
  MIKOS_CHECK(suite, reader.size() == 64);
  MIKOS_CHECK(suite, reader.read(7, output, sizeof(output)));
  for (u32 index = 0; index < sizeof(output); ++index) {
    MIKOS_CHECK(suite, output[index] == index + 7);
  }
  MIKOS_CHECK(suite, device.reads() == 2);

  u8 cached[3]{};
  MIKOS_CHECK(suite, reader.read(17, cached, sizeof(cached)));
  MIKOS_CHECK(suite, device.reads() == 2);
  MIKOS_CHECK(suite, !reader.read(63, output, 2));
  MIKOS_CHECK(suite, !reader.read(0, nullptr, 1));
  const u8 replacement[] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5};
  MIKOS_CHECK(suite, reader.write(14, replacement, sizeof(replacement)));
  u8 written[sizeof(replacement)]{};
  MIKOS_CHECK(suite, reader.read(14, written, sizeof(written)));
  for (u32 index = 0; index < sizeof(written); ++index) {
    MIKOS_CHECK(suite, written[index] == replacement[index]);
  }
  MIKOS_CHECK(suite, reader.flush());
  device.fail(true);
  reader.invalidate();
  MIKOS_CHECK(suite, !reader.read(0, output, 1));
  return suite.finish();
}
