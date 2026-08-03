#pragma once

#include <mikos/base.hpp>

namespace mikos::drivers::storage::root_device {

inline constexpr u32 sector_size = 512;

class Device {
 public:
  static constexpr u32 sector_size = root_device::sector_size;

  [[nodiscard]] bool initialize();
  [[nodiscard]] u64 sector_count() const;
  [[nodiscard]] bool read_sector(u64 sector, u8* output);
  [[nodiscard]] bool read_sectors(u64 sector, u8* output, u32 byte_count);
  [[nodiscard]] bool write_sector(u64 sector, const u8* input);
  [[nodiscard]] bool write_sectors(u64 sector, const u8* input,
                                   u32 byte_count);
  [[nodiscard]] bool flush();
};

}  // namespace mikos::drivers::storage::root_device
