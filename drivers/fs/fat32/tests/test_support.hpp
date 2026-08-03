#pragma once

#include <drivers/fs/fat32/fat32.hpp>

#include <cstring>
#include <unordered_map>

namespace mikos::drivers::fs::fat32::test {

class Device {
 public:
  [[nodiscard]] u64 size() const { return size_; }

  [[nodiscard]] bool read(u64 offset, u8* output, u32 size) {
    if (fail_reads_ || offset > size_ || size > size_ - offset) {
      return false;
    }
    for (u32 index = 0; index < size; ++index) {
      const auto found = bytes_.find(offset + index);
      output[index] =
          found == bytes_.end() ? 0 : found->second;
    }
    return true;
  }

  bool write(u64 offset, const u8* input, u32 size) {
    if (fail_writes_ || offset > size_ || size > size_ - offset) {
      return false;
    }
    for (u32 index = 0; index < size; ++index) {
      if (input[index] == 0) {
        bytes_.erase(offset + index);
      } else {
        bytes_[offset + index] = input[index];
      }
    }
    return true;
  }

  bool flush() {
    ++flush_count_;
    return !fail_flushes_;
  }

  void write8(u64 offset, u8 value) { write(offset, &value, 1); }

  void write16(u64 offset, u16 value) {
    const u8 bytes[2] = {static_cast<u8>(value),
                         static_cast<u8>(value >> 8)};
    write(offset, bytes, sizeof(bytes));
  }

  void write32(u64 offset, u32 value) {
    const u8 bytes[4] = {
        static_cast<u8>(value), static_cast<u8>(value >> 8),
        static_cast<u8>(value >> 16), static_cast<u8>(value >> 24)};
    write(offset, bytes, sizeof(bytes));
  }

  void fail_reads(bool fail) { fail_reads_ = fail; }
  void fail_writes(bool fail) { fail_writes_ = fail; }
  void fail_flushes(bool fail) { fail_flushes_ = fail; }
  [[nodiscard]] u32 flush_count() const { return flush_count_; }
  void resize(u64 size) { size_ = size; }

 private:
  u64 size_{};
  bool fail_reads_{};
  bool fail_writes_{};
  bool fail_flushes_{};
  u32 flush_count_{};
  std::unordered_map<u64, u8> bytes_;
};

class Image {
 public:
  static constexpr u32 bytes_per_sector = 512;
  static constexpr u32 sectors_per_cluster = 1;
  static constexpr u32 reserved_sectors = 32;
  static constexpr u32 fat_count = 1;
  static constexpr u32 cluster_count = 65525;
  static constexpr u32 fat_sectors = 512;
  static constexpr u32 first_data_sector =
      reserved_sectors + fat_count * fat_sectors;
  static constexpr u32 total_sectors =
      first_data_sector + cluster_count;

  Image() {
    device.resize(static_cast<u64>(total_sectors) * bytes_per_sector);
    u8 boot[bytes_per_sector]{};
    boot[0] = 0xeb;
    boot[2] = 0x90;
    put16(boot + 11, bytes_per_sector);
    boot[13] = sectors_per_cluster;
    put16(boot + 14, reserved_sectors);
    boot[16] = fat_count;
    put16(boot + 17, 0);
    put16(boot + 19, 0);
    boot[21] = 0xf8;
    put16(boot + 22, 0);
    put32(boot + 32, total_sectors);
    put32(boot + 36, fat_sectors);
    put16(boot + 40, 0);
    put16(boot + 42, 0);
    put32(boot + 44, 2);
    put16(boot + 48, 1);
    put16(boot + 50, 6);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    device.write(0, boot, sizeof(boot));

    set_fat(0, 0x0ffffff8);
    set_fat(1, 0x0fffffff);
    set_fat(2, 0x0fffffff);
  }

  [[nodiscard]] u64 cluster_offset(u32 cluster) const {
    return static_cast<u64>(
               first_data_sector + (cluster - 2) * sectors_per_cluster) *
           bytes_per_sector;
  }

  void set_fat(u32 cluster, u32 value) {
    device.write32(static_cast<u64>(reserved_sectors) * bytes_per_sector +
                       static_cast<u64>(cluster) * 4,
                   value);
  }

  void write_cluster(u32 cluster, u32 offset, const u8* data, u32 size) {
    device.write(cluster_offset(cluster) + offset, data, size);
  }

  void short_entry(u32 cluster, u32 slot, const char name[11],
                   u8 attributes, u32 first_cluster, u32 size) {
    u8 entry[32]{};
    for (u32 index = 0; index < 11; ++index) {
      entry[index] = static_cast<u8>(name[index]);
    }
    entry[11] = attributes;
    put16(entry + 20, static_cast<u16>(first_cluster >> 16));
    put16(entry + 26, static_cast<u16>(first_cluster));
    put32(entry + 28, size);
    write_cluster(cluster, slot * 32, entry, sizeof(entry));
  }

  void long_entry(u32 cluster, u32 slot, u8 ordinal, u8 checksum,
                  const char16_t* name, u32 name_size) {
    u8 entry[32]{};
    for (u32 index = 0; index < sizeof(entry); ++index) {
      entry[index] = 0xff;
    }
    entry[0] = ordinal;
    entry[11] = 0x0f;
    entry[12] = 0;
    entry[13] = checksum;
    entry[26] = 0;
    entry[27] = 0;
    constexpr u8 offsets[13] = {1,  3,  5,  7,  9,  14, 16,
                                18, 20, 22, 24, 28, 30};
    const u32 part = (ordinal & 0x1f) - 1;
    const u32 base = part * 13;
    for (u32 index = 0; index < 13; ++index) {
      u16 value = 0xffff;
      if (base + index < name_size) {
        value = static_cast<u16>(name[base + index]);
      } else if (base + index == name_size) {
        value = 0;
      }
      put16(entry + offsets[index], value);
    }
    write_cluster(cluster, slot * 32, entry, sizeof(entry));
  }

  void end_directory(u32 cluster, u32 slot) {
    device.write8(cluster_offset(cluster) + slot * 32, 0);
  }

  [[nodiscard]] static u8 checksum(const char name[11]) {
    return detail::short_checksum(
        reinterpret_cast<const u8*>(name));
  }

  Device device;

 private:
  static void put16(u8* output, u16 value) {
    output[0] = static_cast<u8>(value);
    output[1] = static_cast<u8>(value >> 8);
  }

  static void put32(u8* output, u32 value) {
    output[0] = static_cast<u8>(value);
    output[1] = static_cast<u8>(value >> 8);
    output[2] = static_cast<u8>(value >> 16);
    output[3] = static_cast<u8>(value >> 24);
  }
};

}  // namespace mikos::drivers::fs::fat32::test
