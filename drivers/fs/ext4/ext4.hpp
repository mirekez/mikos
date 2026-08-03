#pragma once

#include <drivers/fs/filesystem.hpp>

namespace mikos::drivers::fs::ext4 {

enum class Type : u8 {
  regular,
  directory,
  symbolic_link,
  other,
};

struct Node {
  u32 inode{};
  u16 mode{};
  u16 links{};
  u32 flags{};
  u64 size{};
  u64 allocated_sectors{};
  u32 generation{};
  u64 inode_offset{};
  u64 directory_entry_offset{};
  u8 block_data[60]{};
  Type type{Type::other};

  [[nodiscard]] constexpr bool directory() const {
    return type == Type::directory;
  }
};

struct Entry {
  Name name{};
  Node node{};
  u8 directory_type{};
};

struct Geometry {
  u32 block_size{};
  u64 block_count{};
  u32 first_data_block{};
  u32 blocks_per_group{};
  u32 inodes_per_group{};
  u32 inode_count{};
  u32 inode_size{};
  u32 descriptor_size{};
  u32 group_count{};
  u32 compatible_features{};
  u32 incompatible_features{};
  u32 read_only_features{};
  u64 free_blocks{};
  u32 free_inodes{};
  u32 first_inode{};
  bool metadata_checksums{};
  bool integrity_verified{};
};

namespace detail {

inline constexpr u16 magic = 0xef53;
inline constexpr u16 extent_magic = 0xf30a;
inline constexpr u32 root_inode = 2;
inline constexpr u32 compatible_journal = 0x0004;

inline constexpr u32 incompat_file_type = 0x0002;
inline constexpr u32 incompat_recover = 0x0004;
inline constexpr u32 incompat_meta_bg = 0x0010;
inline constexpr u32 incompat_extents = 0x0040;
inline constexpr u32 incompat_64_bit = 0x0080;
inline constexpr u32 incompat_flex_bg = 0x0200;
inline constexpr u32 incompat_checksum_seed = 0x2000;
inline constexpr u32 incompat_large_directory = 0x4000;
inline constexpr u32 incompat_inline_data = 0x8000;
inline constexpr u32 incompat_encrypt = 0x10000;
inline constexpr u32 incompat_casefold = 0x20000;

inline constexpr u32 read_only_bigalloc = 0x0200;
inline constexpr u32 read_only_metadata_checksum = 0x0400;

inline constexpr u32 inode_extents = 0x00080000;
inline constexpr u32 inode_inline_data = 0x10000000;

inline constexpr u16 mode_type_mask = 0xf000;
inline constexpr u16 mode_directory = 0x4000;
inline constexpr u16 mode_regular = 0x8000;
inline constexpr u16 mode_symbolic_link = 0xa000;

[[nodiscard]] constexpr Type type_from_mode(u16 mode) {
  switch (mode & mode_type_mask) {
    case mode_directory:
      return Type::directory;
    case mode_regular:
      return Type::regular;
    case mode_symbolic_link:
      return Type::symbolic_link;
    default:
      return Type::other;
  }
}

}  // namespace detail

template <ReadableDevice Device>
class Volume {
 public:
  constexpr Volume() = default;

  [[nodiscard]] static Result<Volume> mount(Device& device) {
    Volume volume;
    const Error error = volume.initialize(device);
    return error == Error::none ? Result<Volume>::success(volume)
                                : Result<Volume>::failure(error);
  }

  [[nodiscard]] Error initialize(Device& device) {
    mounted_ = false;
    device_ = &device;
    const Error error =
        read_exact(device, 1024, superblock_, sizeof(superblock_));
    if (error != Error::none) {
      return error;
    }
    const u8* superblock = superblock_;
    if (little_u16(superblock + 0x38) != detail::magic) {
      return Error::invalid_format;
    }

    auto& geometry = geometry_;
    geometry.inode_count = little_u32(superblock + 0x00);
    geometry.first_data_block = little_u32(superblock + 0x14);
    const u32 log_block_size = little_u32(superblock + 0x18);
    geometry.blocks_per_group = little_u32(superblock + 0x20);
    geometry.inodes_per_group = little_u32(superblock + 0x28);
    const u32 revision = little_u32(superblock + 0x4c);
    geometry.inode_size =
        revision == 0 ? 128 : little_u16(superblock + 0x58);
    geometry.compatible_features = little_u32(superblock + 0x5c);
    geometry.incompatible_features = little_u32(superblock + 0x60);
    geometry.read_only_features = little_u32(superblock + 0x64);
    geometry.free_blocks = little_u32(superblock + 0x0c);
    geometry.free_inodes = little_u32(superblock + 0x10);
    geometry.first_inode = revision == 0 ? 11 : little_u32(superblock + 0x54);

    constexpr u32 supported_incompatible =
        detail::incompat_file_type | detail::incompat_recover |
        detail::incompat_extents | detail::incompat_64_bit |
        detail::incompat_flex_bg | detail::incompat_checksum_seed |
        detail::incompat_large_directory;
    if ((geometry.incompatible_features & ~supported_incompatible) != 0 ||
        (geometry.incompatible_features & detail::incompat_meta_bg) != 0 ||
        (geometry.incompatible_features & detail::incompat_inline_data) != 0 ||
        (geometry.incompatible_features & detail::incompat_encrypt) != 0 ||
        (geometry.incompatible_features & detail::incompat_casefold) != 0 ||
        (geometry.read_only_features & detail::read_only_bigalloc) != 0) {
      return Error::unsupported;
    }
    if (log_block_size > 2) {
      return Error::unsupported;
    }
    geometry.block_size = 1024u << log_block_size;
    if (geometry.inode_count < detail::root_inode ||
        geometry.blocks_per_group == 0 ||
        geometry.inodes_per_group == 0 ||
        geometry.inode_size < 128 ||
        geometry.inode_size > geometry.block_size ||
        (geometry.inode_size & 3) != 0) {
      return Error::invalid_format;
    }

    geometry.block_count = little_u32(superblock + 0x04);
    const bool feature_64_bit =
        (geometry.incompatible_features & detail::incompat_64_bit) != 0;
    if (feature_64_bit) {
      geometry.block_count |=
          static_cast<u64>(little_u32(superblock + 0x150)) << 32;
      geometry.descriptor_size = little_u16(superblock + 0xfe);
      if (geometry.descriptor_size < 64 ||
          geometry.descriptor_size > geometry.block_size) {
        return Error::invalid_format;
      }
    } else {
      geometry.descriptor_size = 32;
    }
    if (feature_64_bit) {
      geometry.free_blocks |=
          static_cast<u64>(little_u32(superblock + 0x158)) << 32;
    }
    if (geometry.block_count <= geometry.first_data_block) {
      return Error::invalid_format;
    }
    const u64 data_blocks =
        geometry.block_count - geometry.first_data_block;
    const u64 groups =
        (data_blocks + geometry.blocks_per_group - 1) /
        geometry.blocks_per_group;
    if (groups == 0 || groups > 0xffffffffu) {
      return Error::unsupported;
    }
    geometry.group_count = static_cast<u32>(groups);
    const u64 inode_capacity =
        groups * geometry.inodes_per_group;
    if (inode_capacity < geometry.inode_count ||
        multiply_overflows(geometry.block_count, geometry.block_size) ||
        geometry.block_count * geometry.block_size >
            static_cast<u64>(device.size())) {
      return Error::out_of_bounds;
    }

    geometry.metadata_checksums =
        (geometry.read_only_features &
         detail::read_only_metadata_checksum) != 0;
    geometry.integrity_verified = !geometry.metadata_checksums;
    mounted_ = true;
    return Error::none;
  }

  [[nodiscard]] constexpr const Geometry& geometry() const {
    return geometry_;
  }

  [[nodiscard]] Result<Node> root() {
    return read_inode(detail::root_inode);
  }

  [[nodiscard]] Result<Node> read_inode(u32 inode_number) {
    if (!mounted_ || inode_number == 0 ||
        inode_number > geometry_.inode_count) {
      return Result<Node>::failure(Error::out_of_bounds);
    }
    const u32 group =
        (inode_number - 1) / geometry_.inodes_per_group;
    const u32 index =
        (inode_number - 1) % geometry_.inodes_per_group;
    if (group >= geometry_.group_count) {
      return Result<Node>::failure(Error::corrupt);
    }

    const u64 descriptor_table_block =
        static_cast<u64>(geometry_.first_data_block) + 1;
    const u64 descriptor_offset =
        descriptor_table_block * geometry_.block_size +
        static_cast<u64>(group) * geometry_.descriptor_size;
    u8 descriptor[64]{};
    const u32 descriptor_bytes =
        geometry_.descriptor_size < sizeof(descriptor)
            ? geometry_.descriptor_size
            : sizeof(descriptor);
    Error error = read_exact(*device_, descriptor_offset, descriptor,
                             descriptor_bytes);
    if (error != Error::none) {
      return Result<Node>::failure(error);
    }
    u64 inode_table = little_u32(descriptor + 0x08);
    if ((geometry_.incompatible_features &
         detail::incompat_64_bit) != 0) {
      inode_table |=
          static_cast<u64>(little_u32(descriptor + 0x28)) << 32;
    }
    if (!valid_physical_block(inode_table)) {
      return Result<Node>::failure(Error::corrupt);
    }

    const u64 inode_offset =
        inode_table * geometry_.block_size +
        static_cast<u64>(index) * geometry_.inode_size;
    u8 raw[128]{};
    error = read_exact(*device_, inode_offset, raw, sizeof(raw));
    if (error != Error::none) {
      return Result<Node>::failure(error);
    }
    Node node;
    node.inode = inode_number;
    node.mode = little_u16(raw + 0x00);
    node.size = little_u32(raw + 0x04);
    node.links = little_u16(raw + 0x1a);
    node.allocated_sectors = little_u32(raw + 0x1c);
    node.flags = little_u32(raw + 0x20);
    for (u32 byte = 0; byte < sizeof(node.block_data); ++byte) {
      node.block_data[byte] = raw[0x28 + byte];
    }
    node.generation = little_u32(raw + 0x64);
    node.size |= static_cast<u64>(little_u32(raw + 0x6c)) << 32;
    node.inode_offset = inode_offset;
    node.type = detail::type_from_mode(node.mode);
    if (node.type == Type::other || node.mode == 0) {
      return Result<Node>::failure(Error::corrupt);
    }
    return Result<Node>::success(node);
  }

  template <typename Visitor>
  [[nodiscard]] Error for_each(const Node& directory, Visitor&& visitor) {
    if (!mounted_) {
      return Error::invalid_argument;
    }
    if (!directory.directory()) {
      return Error::not_directory;
    }
    if ((directory.flags & detail::inode_inline_data) != 0) {
      return Error::unsupported;
    }

    const u64 logical_blocks =
        (directory.size + geometry_.block_size - 1) /
        geometry_.block_size;
    for (u64 logical = 0; logical < logical_blocks; ++logical) {
      if (logical > 0xffffffffu) {
        return Error::unsupported;
      }
      const auto mapping =
          map_block(directory, static_cast<u32>(logical));
      if (!mapping) {
        return mapping.error;
      }
      if (mapping.value.zero) {
        continue;
      }
      Error error =
          read_physical_block(mapping.value.physical, directory_block_);
      if (error != Error::none) {
        return error;
      }

      u32 block_bytes = geometry_.block_size;
      const u64 consumed = logical * geometry_.block_size;
      if (directory.size - consumed < block_bytes) {
        block_bytes = static_cast<u32>(directory.size - consumed);
      }
      u32 offset = 0;
      while (offset < block_bytes) {
        if (block_bytes - offset < 8) {
          return Error::corrupt;
        }
        const u8* raw = directory_block_ + offset;
        const u32 inode_number = little_u32(raw);
        const u32 record_size = little_u16(raw + 4);
        const bool has_file_type =
            (geometry_.incompatible_features &
             detail::incompat_file_type) != 0;
        const u32 name_size =
            has_file_type ? raw[6] : little_u16(raw + 6);
        if (record_size < 8 || (record_size & 3) != 0 ||
            record_size > block_bytes - offset ||
            name_size > 255 || name_size > record_size - 8) {
          return Error::corrupt;
        }
        if (inode_number != 0) {
          if (inode_number > geometry_.inode_count) {
            return Error::corrupt;
          }
          Entry entry;
          entry.name.size = static_cast<u16>(name_size);
          for (u32 index = 0; index < name_size; ++index) {
            entry.name.data[index] =
                static_cast<char>(raw[8 + index]);
          }
          entry.name.data[name_size] = '\0';
          entry.directory_type = has_file_type ? raw[7] : 0;
          const auto inode = read_inode(inode_number);
          if (!inode) {
            return inode.error;
          }
          entry.node = inode.value;
          entry.node.directory_entry_offset =
              mapping.value.physical * geometry_.block_size + offset;
          if (!visitor(entry)) {
            return Error::none;
          }
        }
        offset += record_size;
      }
    }
    return Error::none;
  }

  [[nodiscard]] Result<Node> lookup(const Node& directory,
                                    const char* name) {
    if (name == nullptr || *name == '\0') {
      return Result<Node>::failure(Error::invalid_argument);
    }
    Node found{};
    bool matched = false;
    const Error error = for_each(directory, [&](const Entry& entry) {
      if (entry.name.equals(name)) {
        found = entry.node;
        matched = true;
        return false;
      }
      return true;
    });
    if (error != Error::none) {
      return Result<Node>::failure(error);
    }
    return matched ? Result<Node>::success(found)
                   : Result<Node>::failure(Error::not_found);
  }

  [[nodiscard]] Result<Node> lookup_path(const char* path) {
    if (!mounted_ || path == nullptr) {
      return Result<Node>::failure(Error::invalid_argument);
    }
    auto root_node = root();
    if (!root_node) {
      return root_node;
    }
    Node current = root_node.value;
    u32 cursor = 0;
    while (path[cursor] == '/') {
      ++cursor;
    }
    while (path[cursor] != '\0') {
      char component[Name::capacity + 1]{};
      u32 size = 0;
      while (path[cursor] != '\0' && path[cursor] != '/') {
        if (size == Name::capacity) {
          return Result<Node>::failure(Error::invalid_argument);
        }
        component[size++] = path[cursor++];
      }
      component[size] = '\0';
      while (path[cursor] == '/') {
        ++cursor;
      }
      if (size == 1 && component[0] == '.') {
        continue;
      }
      if (!current.directory()) {
        return Result<Node>::failure(Error::not_directory);
      }
      const auto child = lookup(current, component);
      if (!child) {
        return child;
      }
      current = child.value;
    }
    return Result<Node>::success(current);
  }

  [[nodiscard]] Result<u32> read(const Node& file, u64 offset,
                                 u8* output, u32 size) {
    if (!mounted_ || (output == nullptr && size != 0)) {
      return Result<u32>::failure(Error::invalid_argument);
    }
    if (file.directory()) {
      return Result<u32>::failure(Error::is_directory);
    }
    if (file.type != Type::regular) {
      return Result<u32>::failure(Error::unsupported);
    }
    if ((file.flags & detail::inode_inline_data) != 0) {
      return Result<u32>::failure(Error::unsupported);
    }
    if (offset >= file.size || size == 0) {
      return Result<u32>::success(0);
    }
    u64 available = file.size - offset;
    u32 remaining =
        available < size ? static_cast<u32>(available) : size;
    const u32 requested = remaining;

    while (remaining != 0) {
      const u64 logical64 = offset / geometry_.block_size;
      if (logical64 > 0xffffffffu) {
        return Result<u32>::failure(Error::unsupported);
      }
      const u32 within =
          static_cast<u32>(offset % geometry_.block_size);
      const u32 block_available = geometry_.block_size - within;
      u32 count =
          remaining < block_available ? remaining : block_available;
      const auto mapping =
          map_block(file, static_cast<u32>(logical64));
      if (!mapping) {
        return Result<u32>::failure(mapping.error);
      }
      if (mapping.value.zero) {
        zero_bytes(output, count);
      } else {
        if (within == 0 && count == geometry_.block_size) {
          u32 following = 1;
          while (count <= remaining - geometry_.block_size &&
                 logical64 + following <= 0xffffffffu) {
            const auto next = map_block(
                file, static_cast<u32>(logical64 + following));
            if (!next || next.value.zero ||
                next.value.physical !=
                    mapping.value.physical + following) {
              break;
            }
            count += geometry_.block_size;
            ++following;
          }
        }
        const u64 byte_offset =
            mapping.value.physical * geometry_.block_size + within;
        const Error error =
            read_exact(*device_, byte_offset, output, count);
        if (error != Error::none) {
          return Result<u32>::failure(error);
        }
      }
      output += count;
      offset += count;
      remaining -= count;
    }
    return Result<u32>::success(requested);
  }

  [[nodiscard]] constexpr bool writable_format() const {
    return mounted_ && !geometry_.metadata_checksums &&
           (geometry_.compatible_features & detail::compatible_journal) == 0 &&
           (geometry_.incompatible_features & detail::incompat_recover) == 0;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Result<u64> allocate_block() {
    if (!writable_format()) {
      return Result<u64>::failure(Error::unsupported);
    }
    for (u32 group = 0; group < geometry_.group_count; ++group) {
      GroupDescriptor descriptor{};
      Error error = read_group_descriptor(group, descriptor);
      if (error != Error::none) {
        return Result<u64>::failure(error);
      }
      if (descriptor.free_blocks == 0) {
        continue;
      }
      error = read_physical_block(descriptor.block_bitmap, block_);
      if (error != Error::none) {
        return Result<u64>::failure(error);
      }
      const u64 first = geometry_.first_data_block +
                        static_cast<u64>(group) *
                            geometry_.blocks_per_group;
      u32 count = geometry_.blocks_per_group;
      if (geometry_.block_count - first < count) {
        count = static_cast<u32>(geometry_.block_count - first);
      }
      for (u32 bit = 0; bit < count; ++bit) {
        if ((block_[bit / 8] & (1u << (bit % 8))) != 0) {
          continue;
        }
        block_[bit / 8] |= static_cast<u8>(1u << (bit % 8));
        error = write_exact(*device_,
                            descriptor.block_bitmap * geometry_.block_size,
                            block_, geometry_.block_size);
        if (error != Error::none) {
          return Result<u64>::failure(error);
        }
        --descriptor.free_blocks;
        --geometry_.free_blocks;
        error = write_group_free_counts(group, descriptor);
        if (error == Error::none) {
          error = write_superblock_free_counts();
        }
        if (error == Error::none && !device_->flush()) {
          error = Error::io;
        }
        if (error != Error::none) {
          return Result<u64>::failure(error);
        }
        zero_bytes(block_, geometry_.block_size);
        const u64 allocated = first + bit;
        error = write_exact(*device_, allocated * geometry_.block_size,
                            block_, geometry_.block_size);
        if (error != Error::none || !device_->flush()) {
          return Result<u64>::failure(error == Error::none ? Error::io
                                                           : error);
        }
        return Result<u64>::success(allocated);
      }
      return Result<u64>::failure(Error::corrupt);
    }
    return Result<u64>::failure(Error::no_space);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Result<u32> allocate_inode() {
    if (!writable_format()) {
      return Result<u32>::failure(Error::unsupported);
    }
    for (u32 group = 0; group < geometry_.group_count; ++group) {
      GroupDescriptor descriptor{};
      Error error = read_group_descriptor(group, descriptor);
      if (error != Error::none) {
        return Result<u32>::failure(error);
      }
      if (descriptor.free_inodes == 0) {
        continue;
      }
      error = read_physical_block(descriptor.inode_bitmap, block_);
      if (error != Error::none) {
        return Result<u32>::failure(error);
      }
      const u32 first = group * geometry_.inodes_per_group + 1;
      u32 count = geometry_.inodes_per_group;
      if (geometry_.inode_count - first + 1 < count) {
        count = geometry_.inode_count - first + 1;
      }
      for (u32 bit = 0; bit < count; ++bit) {
        const u32 inode = first + bit;
        if (inode < geometry_.first_inode ||
            (block_[bit / 8] & (1u << (bit % 8))) != 0) {
          continue;
        }
        block_[bit / 8] |= static_cast<u8>(1u << (bit % 8));
        error = write_exact(*device_,
                            descriptor.inode_bitmap * geometry_.block_size,
                            block_, geometry_.block_size);
        if (error != Error::none) {
          return Result<u32>::failure(error);
        }
        --descriptor.free_inodes;
        --geometry_.free_inodes;
        error = write_group_free_counts(group, descriptor);
        if (error == Error::none) {
          error = write_superblock_free_counts();
        }
        if (error == Error::none && !device_->flush()) {
          error = Error::io;
        }
        if (error != Error::none) {
          return Result<u32>::failure(error);
        }
        zero_bytes(block_, geometry_.inode_size);
        const u64 inode_offset =
            descriptor.inode_table * geometry_.block_size +
            static_cast<u64>(bit) * geometry_.inode_size;
        error = write_exact(*device_, inode_offset, block_,
                            geometry_.inode_size);
        if (error != Error::none || !device_->flush()) {
          return Result<u32>::failure(error == Error::none ? Error::io
                                                           : error);
        }
        return Result<u32>::success(inode);
      }
      return Result<u32>::failure(Error::corrupt);
    }
    return Result<u32>::failure(Error::no_space);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error release_block(u64 block) {
    if (!writable_format() || !valid_physical_block(block) ||
        block < geometry_.first_data_block) {
      return Error::invalid_argument;
    }
    const u32 group = static_cast<u32>(
        (block - geometry_.first_data_block) / geometry_.blocks_per_group);
    const u32 bit = static_cast<u32>(
        (block - geometry_.first_data_block) % geometry_.blocks_per_group);
    GroupDescriptor descriptor{};
    Error error = read_group_descriptor(group, descriptor);
    if (error != Error::none) {
      return error;
    }
    const u64 inode_table_blocks =
        (static_cast<u64>(geometry_.inodes_per_group) *
             geometry_.inode_size +
         geometry_.block_size - 1) /
        geometry_.block_size;
    if (block == descriptor.block_bitmap ||
        block == descriptor.inode_bitmap ||
        (block >= descriptor.inode_table &&
         block < descriptor.inode_table + inode_table_blocks)) {
      return Error::invalid_argument;
    }
    error = read_physical_block(descriptor.block_bitmap, block_);
    if (error != Error::none) {
      return error;
    }
    const u8 mask = static_cast<u8>(1u << (bit % 8));
    if ((block_[bit / 8] & mask) == 0) {
      return Error::corrupt;
    }
    zero_bytes(directory_block_, geometry_.block_size);
    error = write_exact(*device_, block * geometry_.block_size,
                        directory_block_, geometry_.block_size);
    if (error != Error::none || !device_->flush()) {
      return error == Error::none ? Error::io : error;
    }
    block_[bit / 8] &= static_cast<u8>(~mask);
    error = write_exact(*device_,
                        descriptor.block_bitmap * geometry_.block_size,
                        block_, geometry_.block_size);
    if (error != Error::none) {
      return error;
    }
    ++descriptor.free_blocks;
    ++geometry_.free_blocks;
    error = write_group_free_counts(group, descriptor);
    if (error == Error::none) {
      error = write_superblock_free_counts();
    }
    if (error == Error::none && !device_->flush()) {
      error = Error::io;
    }
    return error;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error release_inode(u32 inode) {
    if (!writable_format() || inode < geometry_.first_inode ||
        inode > geometry_.inode_count) {
      return Error::invalid_argument;
    }
    const u32 group = (inode - 1) / geometry_.inodes_per_group;
    const u32 bit = (inode - 1) % geometry_.inodes_per_group;
    GroupDescriptor descriptor{};
    Error error = read_group_descriptor(group, descriptor);
    if (error != Error::none) {
      return error;
    }
    error = read_physical_block(descriptor.inode_bitmap, block_);
    if (error != Error::none) {
      return error;
    }
    const u8 mask = static_cast<u8>(1u << (bit % 8));
    if ((block_[bit / 8] & mask) == 0) {
      return Error::corrupt;
    }
    zero_bytes(directory_block_, geometry_.inode_size);
    const u64 inode_offset =
        descriptor.inode_table * geometry_.block_size +
        static_cast<u64>(bit) * geometry_.inode_size;
    error = write_exact(*device_, inode_offset, directory_block_,
                        geometry_.inode_size);
    if (error != Error::none || !device_->flush()) {
      return error == Error::none ? Error::io : error;
    }
    block_[bit / 8] &= static_cast<u8>(~mask);
    error = write_exact(*device_,
                        descriptor.inode_bitmap * geometry_.block_size,
                        block_, geometry_.block_size);
    if (error != Error::none) {
      return error;
    }
    ++descriptor.free_inodes;
    ++geometry_.free_inodes;
    error = write_group_free_counts(group, descriptor);
    if (error == Error::none) {
      error = write_superblock_free_counts();
    }
    if (error == Error::none && !device_->flush()) {
      error = Error::io;
    }
    return error;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Result<u32> write(Node& file, u64 offset,
                                  const u8* input, u32 size) {
    if (!writable_format() || (input == nullptr && size != 0) ||
        file.type != Type::regular || offset > 0xffffffffu ||
        size > 0xffffffffu - offset) {
      return Result<u32>::failure(Error::invalid_argument);
    }
    if (size == 0) {
      return Result<u32>::success(0);
    }
    const u64 end = offset + size;
    Error error = ensure_file_blocks(file, end);
    if (error != Error::none) {
      return Result<u32>::failure(error);
    }
    u32 remaining = size;
    while (remaining != 0) {
      const u32 logical = static_cast<u32>(offset / geometry_.block_size);
      const u32 within = static_cast<u32>(offset % geometry_.block_size);
      const u32 count = remaining < geometry_.block_size - within
                            ? remaining
                            : geometry_.block_size - within;
      const auto mapping = map_block(file, logical);
      if (!mapping || mapping.value.zero) {
        return Result<u32>::failure(!mapping ? mapping.error
                                             : Error::corrupt);
      }
      error = write_exact(*device_,
                          mapping.value.physical * geometry_.block_size +
                              within,
                          input, count);
      if (error != Error::none) {
        return Result<u32>::failure(error);
      }
      input += count;
      offset += count;
      remaining -= count;
    }
    if (!device_->flush()) {
      return Result<u32>::failure(Error::io);
    }
    if (end > file.size) {
      file.size = end;
      error = persist_inode(file);
      if (error != Error::none || !device_->flush()) {
        return Result<u32>::failure(error == Error::none ? Error::io
                                                         : error);
      }
    }
    return Result<u32>::success(size);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error truncate(Node& file, u64 size) {
    if (!writable_format() || file.type != Type::regular ||
        size > 0xffffffffu) {
      return Error::invalid_argument;
    }
    if (size == file.size) {
      return Error::none;
    }
    if (size > file.size) {
      Error error = ensure_file_blocks(file, size);
      if (error != Error::none) {
        return error;
      }
      file.size = size;
      error = persist_inode(file);
      if (error == Error::none && !device_->flush()) {
        error = Error::io;
      }
      return error;
    }
    if (size != 0) {
      return Error::unsupported;
    }
    Node old = file;
    file.size = 0;
    file.allocated_sectors = 0;
    file.block_data[2] = 0;
    file.block_data[3] = 0;
    Error error = persist_inode(file);
    if (error != Error::none || !device_->flush()) {
      return error == Error::none ? Error::io : error;
    }
    return release_file_blocks(old);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error sync() {
    return device_->flush() ? Error::none : Error::io;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error create(const char* path, const u8* input, u32 size) {
    if (!writable_format() || path == nullptr ||
        (input == nullptr && size != 0)) {
      return Error::invalid_argument;
    }
    Node parent{};
    char name[256]{};
    Error error = resolve_parent(path, parent, name);
    if (error != Error::none) {
      return error;
    }
    const auto duplicate = lookup(parent, name);
    if (duplicate) {
      return Error::already_exists;
    }
    if (duplicate.error != Error::not_found) {
      return duplicate.error;
    }
    const auto allocated = allocate_inode();
    if (!allocated) {
      return allocated.error;
    }
    Node file{};
    initialize_extent_inode(file, allocated.value, 0x81a4, Type::regular);
    error = persist_inode(file);
    if (error == Error::none && size != 0) {
      const auto stored = write(file, 0, input, size);
      error = stored ? Error::none : stored.error;
    }
    if (error == Error::none) {
      error = insert_directory_entry(parent, name, file);
    }
    if (error != Error::none) {
      (void)release_file_blocks(file);
      (void)release_inode(file.inode);
      return error;
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error mkdir(const char* path, u16 mode) {
    if (!writable_format()) {
      return Error::unsupported;
    }
    if (path == nullptr) {
      return Error::invalid_argument;
    }
    Node parent{};
    char name[256]{};
    Error error = resolve_parent(path, parent, name);
    if (error != Error::none) {
      return error;
    }
    if (!parent.directory()) {
      return Error::not_directory;
    }
    if (parent.links == 0xffff) {
      return Error::no_space;
    }
    const auto duplicate = lookup(parent, name);
    if (duplicate) {
      return Error::already_exists;
    }
    if (duplicate.error != Error::not_found) {
      return duplicate.error;
    }

    const auto allocated_inode = allocate_inode();
    if (!allocated_inode) {
      return allocated_inode.error;
    }
    Node directory{};
    initialize_extent_inode(
        directory, allocated_inode.value,
        static_cast<u16>(detail::mode_directory | (mode & 07777)),
        Type::directory);
    directory.links = 2;

    const auto allocated_block = allocate_block();
    if (!allocated_block) {
      (void)release_inode(directory.inode);
      return allocated_block.error;
    }
    error = append_extent(directory, 0, allocated_block.value);
    if (error == Error::none) {
      directory.size = geometry_.block_size;
      directory.allocated_sectors = geometry_.block_size / 512;
      error = initialize_directory_block(directory, parent,
                                         allocated_block.value);
    }
    if (error == Error::none) {
      error = persist_inode(directory);
    }
    if (error == Error::none) {
      error = insert_directory_entry(parent, name, directory);
    }
    if (error != Error::none) {
      (void)release_block(allocated_block.value);
      (void)release_inode(directory.inode);
      return error;
    }

    ++parent.links;
    error = persist_inode(parent);
    if (error == Error::none) {
      error = increment_used_directories(directory.inode);
    }
    if (error == Error::none && !device_->flush()) {
      error = Error::io;
    }
    return error;
  }

  [[nodiscard]] Result<u32> read(const char* path, u64 offset,
                                 u8* output, u32 size) {
    const auto file = lookup_path(path);
    return file ? read(file.value, offset, output, size)
                : Result<u32>::failure(file.error);
  }

  [[nodiscard]] Result<u32> file_size(const char* path) {
    const auto file = lookup_path(path);
    if (!file) {
      return Result<u32>::failure(file.error);
    }
    return file.value.type == Type::regular
               ? Result<u32>::success(static_cast<u32>(file.value.size))
               : Result<u32>::failure(Error::is_directory);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error remove(const char* path) {
    Node parent{};
    char name[256]{};
    Error error = resolve_parent(path, parent, name);
    if (error != Error::none) {
      return error;
    }
    const auto file = lookup(parent, name);
    if (!file) {
      return file.error;
    }
    if (file.value.directory()) {
      return Error::is_directory;
    }
    error = erase_directory_entry(parent, file.value.directory_entry_offset);
    if (error != Error::none || !device_->flush()) {
      return error == Error::none ? Error::io : error;
    }
    Node released = file.value;
    error = release_file_blocks(released);
    return error == Error::none ? release_inode(released.inode) : error;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error move(const char* source, const char* destination) {
    Node source_parent{};
    Node destination_parent{};
    char source_name[256]{};
    char destination_name[256]{};
    Error error = resolve_parent(source, source_parent, source_name);
    if (error == Error::none) {
      error = resolve_parent(destination, destination_parent,
                             destination_name);
    }
    if (error != Error::none) {
      return error;
    }
    const auto file = lookup(source_parent, source_name);
    if (!file) {
      return file.error;
    }
    const auto duplicate = lookup(destination_parent, destination_name);
    if (duplicate) {
      return Error::already_exists;
    }
    if (duplicate.error != Error::not_found) {
      return duplicate.error;
    }
    error = insert_directory_entry(destination_parent, destination_name,
                                   file.value);
    if (error == Error::none) {
      error = erase_directory_entry(source_parent,
                                    file.value.directory_entry_offset);
    }
    if (error == Error::none && !device_->flush()) {
      error = Error::io;
    }
    return error;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error concatenate(const char* destination,
                                  const char* source) {
    auto target = lookup_path(destination);
    const auto origin = lookup_path(source);
    if (!target) {
      return target.error;
    }
    if (!origin) {
      return origin.error;
    }
    if (target.value.size > 0xffffffffu || origin.value.size > 0xffffffffu ||
        origin.value.size > 0xffffffffu - target.value.size) {
      return Error::no_space;
    }
    const u32 target_size = static_cast<u32>(target.value.size);
    const u32 source_size = static_cast<u32>(origin.value.size);
    u32 copied = 0;
    while (copied < source_size) {
      const u32 count = source_size - copied < geometry_.block_size
                            ? source_size - copied
                            : geometry_.block_size;
      const auto loaded = read(origin.value, copied, directory_block_, count);
      if (!loaded || loaded.value != count) {
        return !loaded ? loaded.error : Error::io;
      }
      const auto stored = write(target.value, target_size + copied,
                                directory_block_, count);
      if (!stored || stored.value != count) {
        return !stored ? stored.error : Error::io;
      }
      copied += count;
    }
    return Error::none;
  }

  [[nodiscard]] bool consistent() {
    const auto root_node = root();
    return root_node &&
           for_each(root_node.value, [](const Entry&) { return true; }) ==
               Error::none;
  }

 private:
  struct GroupDescriptor {
    u64 block_bitmap{};
    u64 inode_bitmap{};
    u64 inode_table{};
    u32 free_blocks{};
    u32 free_inodes{};
    u32 used_directories{};
  };

  struct BlockMapping {
    u64 physical{};
    bool zero{true};
  };

  [[nodiscard]] Error resolve_parent(const char* path, Node& parent,
                                     char* name) {
    if (!mounted_ || path == nullptr || name == nullptr) {
      return Error::invalid_argument;
    }
    u32 length = 0;
    while (path[length] != '\0') {
      if (length == Name::capacity) {
        return Error::invalid_argument;
      }
      ++length;
    }
    while (length != 0 && path[length - 1] == '/') {
      --length;
    }
    u32 start = length;
    while (start != 0 && path[start - 1] != '/') {
      --start;
    }
    const u32 name_size = length - start;
    if (name_size == 0 || name_size > 255 ||
        (name_size == 1 && path[start] == '.') ||
        (name_size == 2 && path[start] == '.' && path[start + 1] == '.')) {
      return Error::invalid_argument;
    }
    for (u32 index = 0; index < name_size; ++index) {
      if (path[start + index] == '/' || path[start + index] == '\0') {
        return Error::invalid_argument;
      }
      name[index] = path[start + index];
    }
    name[name_size] = '\0';
    if (start <= 1) {
      const auto root_node = root();
      if (!root_node) {
        return root_node.error;
      }
      parent = root_node.value;
      return Error::none;
    }
    char parent_path[Name::capacity + 1]{};
    u32 parent_size = start;
    while (parent_size > 1 && path[parent_size - 1] == '/') {
      --parent_size;
    }
    for (u32 index = 0; index < parent_size; ++index) {
      parent_path[index] = path[index];
    }
    const auto found = lookup_path(parent_path);
    if (!found) {
      return found.error;
    }
    if (!found.value.directory()) {
      return Error::not_directory;
    }
    parent = found.value;
    return Error::none;
  }

  void initialize_extent_inode(Node& node, u32 inode, u16 mode, Type type) {
    node = {};
    node.inode = inode;
    node.mode = mode;
    node.links = 1;
    node.flags = detail::inode_extents;
    node.type = type;
    node.block_data[0] = static_cast<u8>(detail::extent_magic);
    node.block_data[1] = static_cast<u8>(detail::extent_magic >> 8);
    node.block_data[4] = 4;
    const u32 group = (inode - 1) / geometry_.inodes_per_group;
    const u32 index = (inode - 1) % geometry_.inodes_per_group;
    GroupDescriptor descriptor{};
    if (read_group_descriptor(group, descriptor) == Error::none) {
      node.inode_offset = descriptor.inode_table * geometry_.block_size +
                          static_cast<u64>(index) * geometry_.inode_size;
    }
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error persist_inode(const Node& node) {
    if (node.inode_offset == 0 || geometry_.inode_size > sizeof(block_)) {
      return Error::corrupt;
    }
    Error error = read_exact(*device_, node.inode_offset, block_,
                             geometry_.inode_size);
    if (error != Error::none) {
      return error;
    }
    auto put16 = [&](u32 offset, u16 value) {
      block_[offset] = static_cast<u8>(value);
      block_[offset + 1] = static_cast<u8>(value >> 8);
    };
    auto put32 = [&](u32 offset, u32 value) {
      block_[offset] = static_cast<u8>(value);
      block_[offset + 1] = static_cast<u8>(value >> 8);
      block_[offset + 2] = static_cast<u8>(value >> 16);
      block_[offset + 3] = static_cast<u8>(value >> 24);
    };
    put16(0x00, node.mode);
    put32(0x04, static_cast<u32>(node.size));
    put16(0x1a, node.links);
    put32(0x1c, static_cast<u32>(node.allocated_sectors));
    put32(0x20, node.flags);
    for (u32 index = 0; index < sizeof(node.block_data); ++index) {
      block_[0x28 + index] = node.block_data[index];
    }
    put32(0x64, node.generation);
    put32(0x6c, static_cast<u32>(node.size >> 32));
    return write_exact(*device_, node.inode_offset, block_,
                       geometry_.inode_size);
  }

  [[nodiscard]] Error append_extent(Node& node, u32 logical, u64 physical) {
    u8* root = node.block_data;
    if (little_u16(root) != detail::extent_magic || little_u16(root + 6) != 0) {
      return Error::unsupported;
    }
    u16 entries = little_u16(root + 2);
    if (entries != 0) {
      u8* last = root + 12 + static_cast<u32>(entries - 1) * 12;
      const u32 first = little_u32(last);
      const u16 length = little_u16(last + 4);
      const u64 start = (static_cast<u64>(little_u16(last + 6)) << 32) |
                        little_u32(last + 8);
      if (first + length == logical && start + length == physical &&
          length < 32768) {
        const u16 grown = length + 1;
        last[4] = static_cast<u8>(grown);
        last[5] = static_cast<u8>(grown >> 8);
        return Error::none;
      }
    }
    if (entries >= 4) {
      return Error::no_space;
    }
    u8* extent = root + 12 + static_cast<u32>(entries) * 12;
    extent[0] = static_cast<u8>(logical);
    extent[1] = static_cast<u8>(logical >> 8);
    extent[2] = static_cast<u8>(logical >> 16);
    extent[3] = static_cast<u8>(logical >> 24);
    extent[4] = 1;
    extent[5] = 0;
    extent[6] = static_cast<u8>(physical >> 32);
    extent[7] = static_cast<u8>(physical >> 40);
    extent[8] = static_cast<u8>(physical);
    extent[9] = static_cast<u8>(physical >> 8);
    extent[10] = static_cast<u8>(physical >> 16);
    extent[11] = static_cast<u8>(physical >> 24);
    ++entries;
    root[2] = static_cast<u8>(entries);
    root[3] = static_cast<u8>(entries >> 8);
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error ensure_file_blocks(Node& node, u64 size) {
    const u32 current = static_cast<u32>(
        (node.size + geometry_.block_size - 1) / geometry_.block_size);
    const u32 required = static_cast<u32>(
        (size + geometry_.block_size - 1) / geometry_.block_size);
    for (u32 logical = current; logical < required; ++logical) {
      const auto block = allocate_block();
      if (!block) {
        return block.error;
      }
      const Error error = append_extent(node, logical, block.value);
      if (error != Error::none) {
        (void)release_block(block.value);
        return error;
      }
      node.allocated_sectors += geometry_.block_size / 512;
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error release_file_blocks(Node& node) {
    if ((node.flags & detail::inode_extents) == 0 ||
        little_u16(node.block_data) != detail::extent_magic ||
        little_u16(node.block_data + 6) != 0) {
      return node.size == 0 ? Error::none : Error::unsupported;
    }
    const u16 entries = little_u16(node.block_data + 2);
    for (u32 index = 0; index < entries; ++index) {
      const u8* extent = node.block_data + 12 + index * 12;
      const u16 length = little_u16(extent + 4);
      const u64 physical =
          (static_cast<u64>(little_u16(extent + 6)) << 32) |
          little_u32(extent + 8);
      for (u32 block = 0; block < length; ++block) {
        const Error error = release_block(physical + block);
        if (error != Error::none) {
          return error;
        }
      }
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error insert_directory_entry(const Node& directory,
                                             const char* name,
                                             const Node& child) {
    u32 name_size = 0;
    while (name[name_size] != '\0' && name_size <= 255) {
      ++name_size;
    }
    if (name_size == 0 || name_size > 255) {
      return Error::invalid_argument;
    }
    const u32 needed = (8 + name_size + 3) & ~u32{3};
    const u32 blocks = static_cast<u32>(
        (directory.size + geometry_.block_size - 1) / geometry_.block_size);
    for (u32 logical = 0; logical < blocks; ++logical) {
      const auto mapping = map_block(directory, logical);
      if (!mapping || mapping.value.zero) {
        return !mapping ? mapping.error : Error::corrupt;
      }
      Error error = read_physical_block(mapping.value.physical,
                                        directory_block_);
      if (error != Error::none) {
        return error;
      }
      for (u32 offset = 0; offset < geometry_.block_size;) {
        u8* raw = directory_block_ + offset;
        const u32 inode = little_u32(raw);
        const u32 record = little_u16(raw + 4);
        if (record < 8 || (record & 3) != 0 ||
            record > geometry_.block_size - offset) {
          return Error::corrupt;
        }
        u32 slot = offset;
        u32 available = record;
        if (inode != 0) {
          const u32 actual = (8 + raw[6] + 3) & ~u32{3};
          if (record < actual || record - actual < needed) {
            offset += record;
            continue;
          }
          raw[4] = static_cast<u8>(actual);
          raw[5] = static_cast<u8>(actual >> 8);
          slot += actual;
          available -= actual;
        } else if (available < needed) {
          offset += record;
          continue;
        }
        u8* entry = directory_block_ + slot;
        for (u32 index = 0; index < available; ++index) {
          entry[index] = 0;
        }
        entry[0] = static_cast<u8>(child.inode);
        entry[1] = static_cast<u8>(child.inode >> 8);
        entry[2] = static_cast<u8>(child.inode >> 16);
        entry[3] = static_cast<u8>(child.inode >> 24);
        entry[4] = static_cast<u8>(available);
        entry[5] = static_cast<u8>(available >> 8);
        entry[6] = static_cast<u8>(name_size);
        entry[7] =
            (geometry_.incompatible_features & detail::incompat_file_type) == 0
                ? 0
                : (child.type == Type::directory
                       ? 2
                       : (child.type == Type::symbolic_link ? 7 : 1));
        for (u32 index = 0; index < name_size; ++index) {
          entry[8 + index] = static_cast<u8>(name[index]);
        }
        error = write_exact(*device_,
                            mapping.value.physical * geometry_.block_size,
                            directory_block_, geometry_.block_size);
        if (error != Error::none || !device_->flush()) {
          return error == Error::none ? Error::io : error;
        }
        return Error::none;
      }
    }
    return Error::no_space;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error initialize_directory_block(const Node& directory,
                                                 const Node& parent,
                                                 u64 block) {
    zero_bytes(directory_block_, geometry_.block_size);
    const bool file_types =
        (geometry_.incompatible_features & detail::incompat_file_type) != 0;
    auto write_entry = [&](u32 offset, u32 inode, u16 record_size,
                           const char* name, u8 name_size) {
      u8* entry = directory_block_ + offset;
      entry[0] = static_cast<u8>(inode);
      entry[1] = static_cast<u8>(inode >> 8);
      entry[2] = static_cast<u8>(inode >> 16);
      entry[3] = static_cast<u8>(inode >> 24);
      entry[4] = static_cast<u8>(record_size);
      entry[5] = static_cast<u8>(record_size >> 8);
      entry[6] = name_size;
      entry[7] = file_types ? 2 : 0;
      for (u32 index = 0; index < name_size; ++index) {
        entry[8 + index] = static_cast<u8>(name[index]);
      }
    };
    write_entry(0, directory.inode, 12, ".", 1);
    write_entry(12, parent.inode,
                static_cast<u16>(geometry_.block_size - 12), "..", 2);
    return write_exact(*device_, block * geometry_.block_size,
                       directory_block_, geometry_.block_size);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error increment_used_directories(u32 inode) {
    const u32 group = (inode - 1) / geometry_.inodes_per_group;
    GroupDescriptor descriptor{};
    Error error = read_group_descriptor(group, descriptor);
    if (error != Error::none) {
      return error;
    }
    if (descriptor.used_directories == 0xffffffffu) {
      return Error::no_space;
    }
    ++descriptor.used_directories;
    return write_group_free_counts(group, descriptor);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error erase_directory_entry(const Node& directory,
                                            u64 entry_offset) {
    if (!directory.directory() || entry_offset == 0) {
      return Error::invalid_argument;
    }
    const u64 block_offset =
        entry_offset - entry_offset % geometry_.block_size;
    Error error = read_exact(*device_, block_offset, directory_block_,
                             geometry_.block_size);
    if (error != Error::none) {
      return error;
    }
    const u32 within = static_cast<u32>(entry_offset - block_offset);
    if (within > geometry_.block_size - 8 ||
        little_u32(directory_block_ + within) == 0) {
      return Error::corrupt;
    }
    for (u32 index = 0; index < 4; ++index) {
      directory_block_[within + index] = 0;
    }
    return write_exact(*device_, block_offset, directory_block_,
                       geometry_.block_size);
  }

  [[nodiscard]] constexpr bool valid_physical_block(u64 block) const {
    return block != 0 && block < geometry_.block_count;
  }

  [[nodiscard]] constexpr u64 group_descriptor_offset(u32 group) const {
    return (static_cast<u64>(geometry_.first_data_block) + 1) *
               geometry_.block_size +
           static_cast<u64>(group) * geometry_.descriptor_size;
  }

  [[nodiscard]] Error read_group_descriptor(u32 group,
                                            GroupDescriptor& output) {
    if (group >= geometry_.group_count) {
      return Error::out_of_bounds;
    }
    u8 raw[64]{};
    const u32 size = geometry_.descriptor_size < sizeof(raw)
                         ? geometry_.descriptor_size
                         : sizeof(raw);
    const Error error =
        read_exact(*device_, group_descriptor_offset(group), raw, size);
    if (error != Error::none) {
      return error;
    }
    output.block_bitmap = little_u32(raw + 0x00);
    output.inode_bitmap = little_u32(raw + 0x04);
    output.inode_table = little_u32(raw + 0x08);
    output.free_blocks = little_u16(raw + 0x0c);
    output.free_inodes = little_u16(raw + 0x0e);
    output.used_directories = little_u16(raw + 0x10);
    if ((geometry_.incompatible_features & detail::incompat_64_bit) != 0) {
      output.block_bitmap |= static_cast<u64>(little_u32(raw + 0x20)) << 32;
      output.inode_bitmap |= static_cast<u64>(little_u32(raw + 0x24)) << 32;
      output.inode_table |= static_cast<u64>(little_u32(raw + 0x28)) << 32;
      output.free_blocks |= static_cast<u32>(little_u16(raw + 0x2c)) << 16;
      output.free_inodes |= static_cast<u32>(little_u16(raw + 0x2e)) << 16;
      output.used_directories |=
          static_cast<u32>(little_u16(raw + 0x30)) << 16;
    }
    if (!valid_physical_block(output.block_bitmap) ||
        !valid_physical_block(output.inode_bitmap) ||
        !valid_physical_block(output.inode_table)) {
      return Error::corrupt;
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error write_group_free_counts(
      u32 group, const GroupDescriptor& descriptor) {
    const u64 offset = group_descriptor_offset(group);
    u8 raw[64]{};
    const u32 size = geometry_.descriptor_size < sizeof(raw)
                         ? geometry_.descriptor_size
                         : sizeof(raw);
    Error error = read_exact(*device_, offset, raw, size);
    if (error != Error::none) {
      return error;
    }
    raw[0x0c] = static_cast<u8>(descriptor.free_blocks);
    raw[0x0d] = static_cast<u8>(descriptor.free_blocks >> 8);
    raw[0x0e] = static_cast<u8>(descriptor.free_inodes);
    raw[0x0f] = static_cast<u8>(descriptor.free_inodes >> 8);
    raw[0x10] = static_cast<u8>(descriptor.used_directories);
    raw[0x11] = static_cast<u8>(descriptor.used_directories >> 8);
    if ((geometry_.incompatible_features & detail::incompat_64_bit) != 0) {
      raw[0x2c] = static_cast<u8>(descriptor.free_blocks >> 16);
      raw[0x2d] = static_cast<u8>(descriptor.free_blocks >> 24);
      raw[0x2e] = static_cast<u8>(descriptor.free_inodes >> 16);
      raw[0x2f] = static_cast<u8>(descriptor.free_inodes >> 24);
      raw[0x30] = static_cast<u8>(descriptor.used_directories >> 16);
      raw[0x31] = static_cast<u8>(descriptor.used_directories >> 24);
    }
    return write_exact(*device_, offset, raw, size);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error write_superblock_free_counts() {
    superblock_[0x0c] = static_cast<u8>(geometry_.free_blocks);
    superblock_[0x0d] = static_cast<u8>(geometry_.free_blocks >> 8);
    superblock_[0x0e] = static_cast<u8>(geometry_.free_blocks >> 16);
    superblock_[0x0f] = static_cast<u8>(geometry_.free_blocks >> 24);
    superblock_[0x10] = static_cast<u8>(geometry_.free_inodes);
    superblock_[0x11] = static_cast<u8>(geometry_.free_inodes >> 8);
    superblock_[0x12] = static_cast<u8>(geometry_.free_inodes >> 16);
    superblock_[0x13] = static_cast<u8>(geometry_.free_inodes >> 24);
    if ((geometry_.incompatible_features & detail::incompat_64_bit) != 0) {
      const u32 high = static_cast<u32>(geometry_.free_blocks >> 32);
      superblock_[0x158] = static_cast<u8>(high);
      superblock_[0x159] = static_cast<u8>(high >> 8);
      superblock_[0x15a] = static_cast<u8>(high >> 16);
      superblock_[0x15b] = static_cast<u8>(high >> 24);
    }
    return write_exact(*device_, 1024, superblock_, sizeof(superblock_));
  }

  [[nodiscard]] Error read_physical_block(u64 block, u8* output) {
    if (!valid_physical_block(block)) {
      return Error::corrupt;
    }
    return read_exact(*device_, block * geometry_.block_size, output,
                      geometry_.block_size);
  }

  [[nodiscard]] Result<u32> indirect_pointer(u32 block, u32 index) {
    if (block == 0) {
      return Result<u32>::success(0);
    }
    Error error = read_physical_block(block, block_);
    if (error != Error::none) {
      return Result<u32>::failure(error);
    }
    const u32 pointers = geometry_.block_size / 4;
    if (index >= pointers) {
      return Result<u32>::failure(Error::corrupt);
    }
    const u32 value = little_u32(block_ + index * 4);
    if (value != 0 && !valid_physical_block(value)) {
      return Result<u32>::failure(Error::corrupt);
    }
    return Result<u32>::success(value);
  }

  [[nodiscard]] Result<BlockMapping> map_legacy(const Node& node,
                                                u32 logical) {
    const u32 pointers = geometry_.block_size / 4;
    if (logical < 12) {
      const u32 block = little_u32(node.block_data + logical * 4);
      if (block != 0 && !valid_physical_block(block)) {
        return Result<BlockMapping>::failure(Error::corrupt);
      }
      return Result<BlockMapping>::success(
          BlockMapping{block, block == 0});
    }
    u64 remaining = logical - 12;
    if (remaining < pointers) {
      const auto block =
          indirect_pointer(little_u32(node.block_data + 48),
                           static_cast<u32>(remaining));
      if (!block) {
        return Result<BlockMapping>::failure(block.error);
      }
      return Result<BlockMapping>::success(
          BlockMapping{block.value, block.value == 0});
    }
    remaining -= pointers;
    const u64 double_capacity =
        static_cast<u64>(pointers) * pointers;
    if (remaining < double_capacity) {
      const auto indirect =
          indirect_pointer(little_u32(node.block_data + 52),
                           static_cast<u32>(remaining / pointers));
      if (!indirect) {
        return Result<BlockMapping>::failure(indirect.error);
      }
      const auto block =
          indirect_pointer(indirect.value,
                           static_cast<u32>(remaining % pointers));
      if (!block) {
        return Result<BlockMapping>::failure(block.error);
      }
      return Result<BlockMapping>::success(
          BlockMapping{block.value, block.value == 0});
    }
    remaining -= double_capacity;
    const u64 triple_capacity = double_capacity * pointers;
    if (remaining >= triple_capacity) {
      return Result<BlockMapping>::failure(Error::out_of_bounds);
    }
    const auto double_block =
        indirect_pointer(little_u32(node.block_data + 56),
                         static_cast<u32>(remaining / double_capacity));
    if (!double_block) {
      return Result<BlockMapping>::failure(double_block.error);
    }
    const u64 within_double = remaining % double_capacity;
    const auto indirect =
        indirect_pointer(double_block.value,
                         static_cast<u32>(within_double / pointers));
    if (!indirect) {
      return Result<BlockMapping>::failure(indirect.error);
    }
    const auto block =
        indirect_pointer(indirect.value,
                         static_cast<u32>(within_double % pointers));
    if (!block) {
      return Result<BlockMapping>::failure(block.error);
    }
    return Result<BlockMapping>::success(
        BlockMapping{block.value, block.value == 0});
  }

  [[nodiscard]] Result<BlockMapping> map_extents(const Node& node,
                                                 u32 logical) {
    const u8* extent_node = node.block_data;
    u32 capacity = sizeof(node.block_data);
    u16 expected_depth = little_u16(extent_node + 6);
    if (expected_depth > 5) {
      return Result<BlockMapping>::failure(Error::corrupt);
    }

    for (;;) {
      if (capacity < 12 ||
          little_u16(extent_node) != detail::extent_magic) {
        return Result<BlockMapping>::failure(Error::corrupt);
      }
      const u32 entries = little_u16(extent_node + 2);
      const u32 maximum = little_u16(extent_node + 4);
      const u32 depth = little_u16(extent_node + 6);
      const u32 capacity_entries = (capacity - 12) / 12;
      if (depth != expected_depth || entries > maximum ||
          maximum > capacity_entries) {
        return Result<BlockMapping>::failure(Error::corrupt);
      }

      if (depth == 0) {
        u64 previous_end = 0;
        bool have_previous = false;
        for (u32 index = 0; index < entries; ++index) {
          const u8* extent = extent_node + 12 + index * 12;
          const u32 first = little_u32(extent);
          const u32 raw_length = little_u16(extent + 4);
          if (raw_length == 0) {
            return Result<BlockMapping>::failure(Error::corrupt);
          }
          const bool uninitialized = raw_length > 32768;
          const u32 length =
              uninitialized ? raw_length - 32768 : raw_length;
          const u64 end = static_cast<u64>(first) + length;
          if ((have_previous && first < previous_end) ||
              end > u64{0x100000000}) {
            return Result<BlockMapping>::failure(Error::corrupt);
          }
          have_previous = true;
          previous_end = end;
          if (logical < first || logical >= end) {
            continue;
          }
          if (uninitialized) {
            return Result<BlockMapping>::success(BlockMapping{});
          }
          const u64 physical =
              (static_cast<u64>(little_u16(extent + 6)) << 32) |
              little_u32(extent + 8);
          const u64 mapped = physical + (logical - first);
          if (!valid_physical_block(mapped)) {
            return Result<BlockMapping>::failure(Error::corrupt);
          }
          return Result<BlockMapping>::success(
              BlockMapping{mapped, false});
        }
        return Result<BlockMapping>::success(BlockMapping{});
      }

      const u8* selected = nullptr;
      u32 previous = 0;
      for (u32 index = 0; index < entries; ++index) {
        const u8* extent_index =
            extent_node + 12 + index * 12;
        const u32 first = little_u32(extent_index);
        if (index != 0 && first <= previous) {
          return Result<BlockMapping>::failure(Error::corrupt);
        }
        previous = first;
        if (first <= logical) {
          selected = extent_index;
        } else {
          break;
        }
      }
      if (selected == nullptr) {
        return Result<BlockMapping>::success(BlockMapping{});
      }
      const u64 child =
          (static_cast<u64>(little_u16(selected + 8)) << 32) |
          little_u32(selected + 4);
      const Error error = read_physical_block(child, block_);
      if (error != Error::none) {
        return Result<BlockMapping>::failure(error);
      }
      extent_node = block_;
      capacity = geometry_.block_size;
      --expected_depth;
    }
  }

  [[nodiscard]] Result<BlockMapping> map_block(const Node& node,
                                               u32 logical) {
    if ((node.flags & detail::inode_inline_data) != 0) {
      return Result<BlockMapping>::failure(Error::unsupported);
    }
    return (node.flags & detail::inode_extents) != 0
               ? map_extents(node, logical)
               : map_legacy(node, logical);
  }

  Device* device_{};
  Geometry geometry_{};
  bool mounted_{};
  u8 superblock_[1024]{};
  u8 block_[4096]{};
  u8 directory_block_[4096]{};
};

}  // namespace mikos::drivers::fs::ext4
