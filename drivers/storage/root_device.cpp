#include <drivers/storage/root_device.hpp>

#ifdef MIKOS_TRIBE
#include <drivers/storage/tribe_sd.hpp>
#else
#include <drivers/storage/virtio_block.hpp>
#endif

namespace mikos::drivers::storage::root_device {

bool Device::initialize() {
#ifdef MIKOS_TRIBE
  return tribe_sd::initialize();
#else
  return virtio_block::initialize();
#endif
}

u64 Device::sector_count() const {
#ifdef MIKOS_TRIBE
  return tribe_sd::sector_count();
#else
  return virtio_block::sector_count();
#endif
}

bool Device::read_sector(u64 sector, u8* output) {
#ifdef MIKOS_TRIBE
  // SectorReader reuses its one-sector cache. Keep those fills on the
  // CPU-visible PIO path: Tribe's DMA completion can be observed before its
  // external D-cache invalidation has retired, exposing stale cache contents.
  // Larger aligned reads still use the DMA path in read_sectors().
  return sector <= 0xffffffffu &&
         tribe_sd::read_block(static_cast<u32>(sector), output);
#else
  return read_sectors(sector, output, sector_size);
#endif
}

bool Device::read_sectors(u64 sector, u8* output, u32 byte_count) {
#ifdef MIKOS_TRIBE
#ifdef MIKOS_TRIBE_MULTICORE
  // Tribe signals SD DMA completion before its external cache invalidation is
  // guaranteed to have retired on every core. Reused kernel buffers can
  // therefore expose data from an earlier read. Keep those immediately reused
  // metadata buffers on the coherent PIO path while retaining the faster DMA
  // path for user-image transfers.
  constexpr usize user_memory_begin = 0x81000000;
  if (reinterpret_cast<usize>(output) < user_memory_begin) {
    if (output == nullptr || byte_count == 0 ||
        byte_count % sector_size != 0 || sector > 0xffffffffu) {
      return false;
    }
    const u64 count = byte_count / sector_size;
    if (count > (u64{1} << 32) - sector) {
      return false;
    }
    for (u64 index = 0; index < count; ++index) {
      if (!tribe_sd::read_block(static_cast<u32>(sector + index),
                                output + index * sector_size)) {
        return false;
      }
    }
    return true;
  }
#endif
  return sector <= 0xffffffffu &&
         tribe_sd::read_blocks(static_cast<u32>(sector), output,
                               byte_count);
#else
  return virtio_block::read_sectors(sector, output, byte_count);
#endif
}

bool Device::write_sector(u64 sector, const u8* input) {
  return write_sectors(sector, input, sector_size);
}

bool Device::write_sectors(u64 sector, const u8* input, u32 byte_count) {
#ifdef MIKOS_TRIBE
  return sector <= 0xffffffffu &&
         tribe_sd::write_blocks(static_cast<u32>(sector), input, byte_count);
#else
  return virtio_block::write_sectors(sector, input, byte_count);
#endif
}

bool Device::flush() {
#ifdef MIKOS_TRIBE
  return tribe_sd::flush();
#else
  return virtio_block::flush();
#endif
}

}  // namespace mikos::drivers::storage::root_device
