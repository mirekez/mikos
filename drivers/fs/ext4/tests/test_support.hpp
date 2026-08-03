#pragma once

#include <drivers/fs/ext4/ext4.hpp>

#include <unordered_map>

namespace mikos::drivers::fs::ext4::test {

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

  void resize(u64 size) { size_ = size; }
  void fail_reads(bool fail) { fail_reads_ = fail; }
  void fail_writes(bool fail) { fail_writes_ = fail; }
  void fail_flushes(bool fail) { fail_flushes_ = fail; }
  [[nodiscard]] u32 flush_count() const { return flush_count_; }

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
  static constexpr u32 block_size = 1024;
  static constexpr u32 block_count = 64;
  static constexpr u32 inode_count = 32;
  static constexpr u32 inodes_per_group = 32;
  static constexpr u32 inode_size = 128;
  static constexpr u32 inode_table_block = 5;

  Image() {
    device.resize(static_cast<u64>(block_count) * block_size);
    u8 superblock[1024]{};
    put32(superblock + 0x00, inode_count);
    put32(superblock + 0x04, block_count);
    put32(superblock + 0x0c, block_count - 11);
    put32(superblock + 0x10, inode_count - 10);
    put32(superblock + 0x14, 1);
    put32(superblock + 0x18, 0);
    put32(superblock + 0x1c, 0);
    put32(superblock + 0x20, block_count);
    put32(superblock + 0x24, block_count);
    put32(superblock + 0x28, inodes_per_group);
    put16(superblock + 0x38, 0xef53);
    put32(superblock + 0x4c, 1);
    put32(superblock + 0x54, 11);
    put16(superblock + 0x58, inode_size);
    put32(superblock + 0x60, 0x42);
    put16(superblock + 0xfe, 32);
    device.write(1024, superblock, sizeof(superblock));
    device.write32(2 * block_size + 0x00, 3);
    device.write32(2 * block_size + 0x04, 4);
    device.write32(2 * block_size + 0x08, inode_table_block);
    device.write16(2 * block_size + 0x0c, block_count - 11);
    device.write16(2 * block_size + 0x0e, inode_count - 10);
    device.write16(2 * block_size + 0x10, 1);
    u8 block_bitmap[block_size]{};
    for (u32 block = 1; block <= 10; ++block) {
      const u32 bit = block - 1;
      block_bitmap[bit / 8] |= static_cast<u8>(1u << (bit % 8));
    }
    device.write(3 * block_size, block_bitmap, sizeof(block_bitmap));
    u8 inode_bitmap[block_size]{};
    inode_bitmap[0] = 0xff;
    inode_bitmap[1] = 0x03;
    device.write(4 * block_size, inode_bitmap, sizeof(inode_bitmap));
    extent_inode(2, 0x41ed, block_size, 10, 1);
    directory_entry(10, 0, 2, 12, ".", 2);
    directory_entry(10, 12, 2, block_size - 12, "..", 2);
  }

  [[nodiscard]] u64 block_offset(u32 block) const {
    return static_cast<u64>(block) * block_size;
  }

  [[nodiscard]] u64 inode_offset(u32 inode) const {
    return block_offset(inode_table_block) +
           static_cast<u64>(inode - 1) * inode_size;
  }

  void extent_inode(u32 inode, u16 mode, u64 size, u32 physical,
                    u16 length, u32 logical = 0,
                    bool uninitialized = false) {
    u8 raw[inode_size]{};
    put16(raw + 0x00, mode);
    put16(raw + 0x1a, (mode & 0xf000) == 0x4000 ? 2 : 1);
    put32(raw + 0x1c,
          static_cast<u32>(length) * block_size / 512);
    put32(raw + 0x04, static_cast<u32>(size));
    put32(raw + 0x20, 0x00080000);
    put16(raw + 0x28, 0xf30a);
    put16(raw + 0x2a, 1);
    put16(raw + 0x2c, 4);
    put16(raw + 0x2e, 0);
    put32(raw + 0x34, logical);
    put16(raw + 0x38,
          static_cast<u16>(length + (uninitialized ? 32768 : 0)));
    put16(raw + 0x3a, 0);
    put32(raw + 0x3c, physical);
    put32(raw + 0x6c, static_cast<u32>(size >> 32));
    device.write(inode_offset(inode), raw, sizeof(raw));
  }

  void two_extent_inode(u32 inode, u16 mode, u64 size,
                        u32 first_logical, u32 first_physical,
                        u16 first_length, u32 second_logical,
                        u32 second_physical, u16 second_length) {
    u8 raw[inode_size]{};
    put16(raw + 0x00, mode);
    put32(raw + 0x04, static_cast<u32>(size));
    put32(raw + 0x20, 0x00080000);
    put16(raw + 0x28, 0xf30a);
    put16(raw + 0x2a, 2);
    put16(raw + 0x2c, 4);
    put16(raw + 0x2e, 0);
    put32(raw + 0x34, first_logical);
    put16(raw + 0x38, first_length);
    put32(raw + 0x3c, first_physical);
    put32(raw + 0x40, second_logical);
    put16(raw + 0x44, second_length);
    put32(raw + 0x48, second_physical);
    put32(raw + 0x6c, static_cast<u32>(size >> 32));
    device.write(inode_offset(inode), raw, sizeof(raw));
  }

  void external_extent_inode(u32 inode, u16 mode, u64 size,
                             u32 tree_block, u32 physical) {
    u8 raw[inode_size]{};
    put16(raw + 0x00, mode);
    put32(raw + 0x04, static_cast<u32>(size));
    put32(raw + 0x20, 0x00080000);
    put16(raw + 0x28, 0xf30a);
    put16(raw + 0x2a, 1);
    put16(raw + 0x2c, 4);
    put16(raw + 0x2e, 1);
    put32(raw + 0x34, 0);
    put32(raw + 0x38, tree_block);
    device.write(inode_offset(inode), raw, sizeof(raw));

    u8 tree[block_size]{};
    put16(tree + 0x00, 0xf30a);
    put16(tree + 0x02, 1);
    put16(tree + 0x04, (block_size - 12) / 12);
    put16(tree + 0x06, 0);
    put32(tree + 0x0c, 0);
    put16(tree + 0x10, 1);
    put32(tree + 0x14, physical);
    device.write(block_offset(tree_block), tree, sizeof(tree));
  }

  void legacy_inode(u32 inode, u16 mode, u64 size, u32 direct,
                    u32 indirect, u32 indirect_data) {
    u8 raw[inode_size]{};
    put16(raw + 0x00, mode);
    put32(raw + 0x04, static_cast<u32>(size));
    put32(raw + 0x28, direct);
    put32(raw + 0x28 + 12 * 4, indirect);
    put32(raw + 0x6c, static_cast<u32>(size >> 32));
    device.write(inode_offset(inode), raw, sizeof(raw));
    if (indirect != 0) {
      device.write32(block_offset(indirect), indirect_data);
    }
  }

  void directory_entry(u32 block, u32 offset, u32 inode,
                       u16 record_size, const char* name,
                       u8 file_type) {
    u8 entry[264]{};
    u32 name_size = 0;
    while (name[name_size] != '\0') {
      ++name_size;
    }
    put32(entry, inode);
    put16(entry + 4, record_size);
    entry[6] = static_cast<u8>(name_size);
    entry[7] = file_type;
    for (u32 index = 0; index < name_size; ++index) {
      entry[8 + index] = static_cast<u8>(name[index]);
    }
    device.write(block_offset(block) + offset, entry, 8 + name_size);
  }

  void write_block(u32 block, const u8* data, u32 size,
                   u32 offset = 0) {
    device.write(block_offset(block) + offset, data, size);
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

}  // namespace mikos::drivers::fs::ext4::test
