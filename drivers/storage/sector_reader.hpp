#pragma once

#include <mikos/base.hpp>
#include <algorithm>
#include <array>
#include <span>

namespace mikos::drivers::storage {

template <typename Device>
concept SectorDevice = requires(Device& device, u64 sector, u8* output) {
  Device::sector_size;
  device.sector_count();
  device.read_sector(sector, output);
  device.read_sectors(sector, output, u32{});
};

template <typename Device>
concept WritableSectorDevice = SectorDevice<Device> &&
    requires(Device& device, u64 sector, const u8* input, u32 size) {
      device.write_sector(sector, input);
      device.write_sectors(sector, input, size);
      device.flush();
    };

template <SectorDevice Device>
class SectorReader {
 public:
  explicit constexpr SectorReader(Device& device) : device_{&device} {}

  [[nodiscard]] bool read(u64 offset, std::span<u8> output) {
    return output.size() <= ~u32{0} &&
           read(offset, output.data(), static_cast<u32>(output.size()));
  }

  [[nodiscard]] bool write(u64 offset, std::span<const u8> input)
      requires WritableSectorDevice<Device> {
    return input.size() <= ~u32{0} &&
           write(offset, input.data(), static_cast<u32>(input.size()));
  }

  [[nodiscard]] u64 size() const {
    const u64 sectors = device_->sector_count();
    return sectors > ~u64{0} / Device::sector_size
               ? ~u64{0}
               : sectors * Device::sector_size;
  }

  [[nodiscard]] bool read(u64 offset, u8* output, u32 count) {
    if ((output == nullptr && count != 0) || offset > size() ||
        count > size() - offset) {
      return false;
    }
    while (count != 0) {
      const u64 sector = offset / Device::sector_size;
      const u32 within = static_cast<u32>(offset % Device::sector_size);
      if (within == 0 && count >= Device::sector_size &&
          (reinterpret_cast<usize>(output) & 7u) == 0) {
        const u32 direct =
            count - count % Device::sector_size;
        if (!device_->read_sectors(sector, output, direct)) {
          cache_valid_ = false;
          return false;
        }
        cache_valid_ = false;
        output += direct;
        offset += direct;
        count -= direct;
        continue;
      }
      u32 chunk = Device::sector_size - within;
      if (chunk > count) {
        chunk = count;
      }
      if (!cache_valid_ || cached_sector_ != sector) {
        if (!device_->read_sector(sector, cache_.data())) {
          cache_valid_ = false;
          return false;
        }
        cached_sector_ = sector;
        cache_valid_ = true;
      }
      std::copy_n(cache_.data() + within, chunk, output);
      output += chunk;
      offset += chunk;
      count -= chunk;
    }
    return true;
  }

  [[nodiscard]] bool write(u64 offset, const u8* input, u32 count)
      requires WritableSectorDevice<Device> {
    if ((input == nullptr && count != 0) || offset > size() ||
        count > size() - offset) {
      return false;
    }
    while (count != 0) {
      const u64 sector = offset / Device::sector_size;
      const u32 within = static_cast<u32>(offset % Device::sector_size);
      if (within == 0 && count >= Device::sector_size &&
          (reinterpret_cast<usize>(input) & 7u) == 0) {
        const u32 direct = count - count % Device::sector_size;
        if (!device_->write_sectors(sector, input, direct)) {
          cache_valid_ = false;
          return false;
        }
        cache_valid_ = false;
        input += direct;
        offset += direct;
        count -= direct;
        continue;
      }
      u32 chunk = Device::sector_size - within;
      if (chunk > count) {
        chunk = count;
      }
      if (!cache_valid_ || cached_sector_ != sector) {
        if (!device_->read_sector(sector, cache_.data())) {
          cache_valid_ = false;
          return false;
        }
        cached_sector_ = sector;
        cache_valid_ = true;
      }
      std::copy_n(input, chunk, cache_.data() + within);
      if (!device_->write_sector(sector, cache_.data())) {
        cache_valid_ = false;
        return false;
      }
      input += chunk;
      offset += chunk;
      count -= chunk;
    }
    return true;
  }

  [[nodiscard]] bool flush() requires WritableSectorDevice<Device> {
    return device_->flush();
  }

  void invalidate() { cache_valid_ = false; }

 private:
  Device* device_{};
  alignas(8) std::array<u8, Device::sector_size> cache_{};
  u64 cached_sector_{};
  bool cache_valid_{};
};

}  // namespace mikos::drivers::storage
