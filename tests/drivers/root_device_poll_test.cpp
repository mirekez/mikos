#include <drivers/storage/root_device.hpp>
#include <drivers/storage/tribe_sd.hpp>
#include <support/test.hpp>

#include <array>
#include <sys/mman.h>

namespace {

using namespace mikos;

struct Transfer {
  u32 block{};
  u32 size{};
  u32 polls{};
  bool dma{};
};

std::array<Transfer, 32> transfers{};
u32 transfer_count{};
u32 poll_count{};
u32 fail_transfer{};

void reset() {
  transfers = {};
  transfer_count = poll_count = fail_transfer = 0;
}

bool read(u32 block, u8* output, u32 size, bool dma) {
  transfers[transfer_count++] = {block, size, poll_count, dma};
  if (transfer_count == fail_transfer) return false;
  for (u32 byte = 0; byte < size; ++byte) {
    output[byte] = static_cast<u8>(block + byte / 512);
  }
  return true;
}

}  // namespace

namespace mikos::network {
void poll() { ++poll_count; }
}  // namespace mikos::network

namespace mikos::drivers::tribe_sd {
bool initialize() { return true; }
u64 sector_count() { return u64{1} << 32; }
bool read_block(u32 block, u8* output) {
  return read(block, output, block_size, false);
}
bool read_blocks(u32 block, u8* output, u32 count) {
  if (output == nullptr || (reinterpret_cast<usize>(output) & 7u) != 0 ||
      count == 0 || count % block_size != 0 ||
      count / block_size > 0xffffffffu - block) return false;
  return read(block, output, count, true);
}
bool write_blocks(u32, const u8*, u32) { return true; }
bool flush() { return true; }
}  // namespace mikos::drivers::tribe_sd

int main() {
  using namespace mikos;
  test::Suite suite{"drivers/root_device_poll"};
  drivers::storage::root_device::Device device;
  alignas(8) std::array<u8, 3 * 4096 + 512> image{};

  // One coalesced filesystem read must leave opportunities for TCP progress
  // throughout the transfer, while preserving the complete image bytes.
  MIKOS_CHECK(suite, device.read_sectors(10, image.data(), image.size()));
  u32 transferred = 0;
  for (u32 index = 0; index < transfer_count; ++index) {
    MIKOS_CHECK(suite, transfers[index].dma);
    MIKOS_CHECK(suite, transfers[index].size != 0 &&
                           transfers[index].size <= 4096);
    MIKOS_CHECK(suite, transfers[index].size % 512 == 0);
    MIKOS_CHECK(suite, transfers[index].block == 10 + transferred / 512);
    MIKOS_CHECK(suite, transfers[index].polls >
                           (index == 0 ? 0 : transfers[index - 1].polls));
    transferred += transfers[index].size;
  }
  MIKOS_CHECK(suite, transferred == image.size());
  for (u32 byte = 0; byte < image.size(); ++byte) {
    MIKOS_CHECK(suite, image[byte] == static_cast<u8>(10 + byte / 512));
  }

  reset();
  image.fill(0xa5);
  fail_transfer = 2;
  MIKOS_CHECK(suite, !device.read_sectors(10, image.data(), image.size()));
  MIKOS_CHECK(suite, transfer_count == 2);
  const u32 completed = transfers[0].size;
  MIKOS_CHECK(suite, completed != 0 && completed < image.size());
  if (completed != 0 && completed < image.size()) {
    MIKOS_CHECK(suite, image[completed - 1] == 10 + (completed - 1) / 512);
    MIKOS_CHECK(suite, image[completed] == 0xa5);
  }
  MIKOS_CHECK(suite, image.back() == 0xa5);

  reset();
  MIKOS_CHECK(suite, !device.read_sectors(0, image.data(), 511));
  MIKOS_CHECK(suite, !device.read_sectors(0xffffffffu, image.data(), 1024));
  MIKOS_CHECK(suite, !device.read_sectors(u64{1} << 32, image.data(), 512));
  MIKOS_CHECK(suite, transfer_count == 0);

  // SectorReader's cache fills always use PIO, including on a single core.
  MIKOS_CHECK(suite, device.read_sector(7, image.data()));
  MIKOS_CHECK(suite, transfer_count == 1 && !transfers[0].dma);
  MIKOS_CHECK(suite, transfers[0].polls != 0);
  MIKOS_CHECK(suite, image[0] == 7 && image[511] == 7);

  // Multicore metadata buffers below user memory must poll between PIO
  // sectors too. Reserve a free address without replacing any host mapping.
  auto* metadata = static_cast<u8*>(mmap(
      reinterpret_cast<void*>(0x40000000), 4096, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
  MIKOS_CHECK(suite, metadata != MAP_FAILED);
  if (metadata != MAP_FAILED) {
    reset();
    MIKOS_CHECK(suite, device.read_sectors(30, metadata, 1536));
    MIKOS_CHECK(suite, transfer_count == 3);
    for (u32 index = 0; index < transfer_count; ++index) {
      MIKOS_CHECK(suite, !transfers[index].dma);
      MIKOS_CHECK(suite, transfers[index].size == 512);
      MIKOS_CHECK(suite, transfers[index].polls >
                             (index == 0 ? 0 : transfers[index - 1].polls));
      MIKOS_CHECK(suite, metadata[index * 512] == 30 + index);
    }
    munmap(metadata, 4096);
  }
  return suite.finish();
}
