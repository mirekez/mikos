#pragma once

#include <drivers/fs/filesystem.hpp>

namespace mikos::drivers::fs::fat32 {

enum class Type : u8 {
  file,
  directory,
};

struct Node {
  u32 first_cluster{};
  u32 size{};
  Type type{Type::file};
  u8 attributes{};
  u64 entry_offset{};

  [[nodiscard]] constexpr bool directory() const {
    return type == Type::directory;
  }
};

struct Entry {
  Name name{};
  Name short_name{};
  Node node{};
};

struct Geometry {
  u32 bytes_per_sector{};
  u32 sectors_per_cluster{};
  u32 bytes_per_cluster{};
  u32 reserved_sectors{};
  u32 fat_sectors{};
  u32 fat_count{};
  u32 active_fat{};
  u32 first_data_sector{};
  u32 cluster_count{};
  u32 root_cluster{};
  u32 total_sectors{};
  u32 fs_info_sector{};
  bool mirror_fats{};
};

namespace detail {

inline constexpr u8 attribute_volume = 0x08;
inline constexpr u8 attribute_directory = 0x10;
inline constexpr u8 attribute_long_name = 0x0f;
inline constexpr u32 fat_mask = 0x0fffffff;
inline constexpr u32 bad_cluster = 0x0ffffff7;
inline constexpr u32 end_of_chain = 0x0ffffff8;
inline constexpr u32 reserved_cluster = 0x0ffffff0;

struct LongNameState {
  u16 characters[260]{};
  u8 checksum{};
  u8 expected{};
  u8 parts{};
  bool active{};

  void reset() {
    for (auto& character : characters) {
      character = 0xffff;
    }
    checksum = 0;
    expected = 0;
    parts = 0;
    active = false;
  }
};

[[nodiscard]] constexpr u8 short_checksum(const u8* name) {
  u8 sum = 0;
  for (u32 index = 0; index < 11; ++index) {
    sum = static_cast<u8>(((sum & 1) != 0 ? 0x80 : 0) + (sum >> 1) +
                          name[index]);
  }
  return sum;
}

inline void append_utf8(Name& name, u32 codepoint, bool& valid) {
  auto append = [&](u8 byte) {
    if (name.size >= Name::capacity) {
      valid = false;
      return;
    }
    name.data[name.size++] = static_cast<char>(byte);
  };

  if (codepoint <= 0x7f) {
    append(static_cast<u8>(codepoint));
  } else if (codepoint <= 0x7ff) {
    append(static_cast<u8>(0xc0 | (codepoint >> 6)));
    append(static_cast<u8>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    append(static_cast<u8>(0xe0 | (codepoint >> 12)));
    append(static_cast<u8>(0x80 | ((codepoint >> 6) & 0x3f)));
    append(static_cast<u8>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0x10ffff) {
    append(static_cast<u8>(0xf0 | (codepoint >> 18)));
    append(static_cast<u8>(0x80 | ((codepoint >> 12) & 0x3f)));
    append(static_cast<u8>(0x80 | ((codepoint >> 6) & 0x3f)));
    append(static_cast<u8>(0x80 | (codepoint & 0x3f)));
  } else {
    valid = false;
  }
}

[[nodiscard]] inline bool decode_long_name(const LongNameState& state,
                                           Name& output) {
  output = {};
  bool valid = state.active && state.expected == 0 && state.parts != 0;
  const u32 count = static_cast<u32>(state.parts) * 13;
  for (u32 index = 0; valid && index < count; ++index) {
    const u16 first = state.characters[index];
    if (first == 0 || first == 0xffff) {
      break;
    }
    u32 codepoint = first;
    if (first >= 0xd800 && first <= 0xdbff) {
      if (++index >= count) {
        valid = false;
        break;
      }
      const u16 second = state.characters[index];
      if (second < 0xdc00 || second > 0xdfff) {
        valid = false;
        break;
      }
      codepoint = 0x10000 +
                  ((static_cast<u32>(first) - 0xd800) << 10) +
                  (static_cast<u32>(second) - 0xdc00);
    } else if (first >= 0xdc00 && first <= 0xdfff) {
      valid = false;
      break;
    }
    append_utf8(output, codepoint, valid);
  }
  output.data[output.size] = '\0';
  return valid && !output.empty();
}

inline void decode_short_name(const u8* entry, Name& output) {
  output = {};
  const bool lowercase_base = (entry[12] & 0x08) != 0;
  const bool lowercase_extension = (entry[12] & 0x10) != 0;
  auto append = [&](u8 value, bool lowercase) {
    if (value == 0x05 && output.size == 0) {
      value = 0xe5;
    }
    if (lowercase && value >= 'A' && value <= 'Z') {
      value = static_cast<u8>(value + ('a' - 'A'));
    }
    output.data[output.size++] = static_cast<char>(value);
  };

  u32 base_size = 8;
  while (base_size != 0 && entry[base_size - 1] == ' ') {
    --base_size;
  }
  for (u32 index = 0; index < base_size; ++index) {
    append(entry[index], lowercase_base);
  }

  u32 extension_size = 3;
  while (extension_size != 0 && entry[8 + extension_size - 1] == ' ') {
    --extension_size;
  }
  if (extension_size != 0) {
    output.data[output.size++] = '.';
    for (u32 index = 0; index < extension_size; ++index) {
      append(entry[8 + index], lowercase_extension);
    }
  }
  output.data[output.size] = '\0';
}

inline void consume_long_entry(const u8* entry, LongNameState& state) {
  const u8 raw_ordinal = entry[0];
  const u8 ordinal = raw_ordinal & 0x1f;
  const bool last = (raw_ordinal & 0x40) != 0;
  if (ordinal == 0 || ordinal > 20 || entry[11] != attribute_long_name ||
      entry[12] != 0 || little_u16(entry + 26) != 0) {
    state.reset();
    return;
  }
  if (last) {
    state.reset();
    state.active = true;
    state.parts = ordinal;
    state.expected = ordinal;
    state.checksum = entry[13];
  }
  if (!state.active || ordinal != state.expected ||
      entry[13] != state.checksum || ordinal > state.parts) {
    state.reset();
    return;
  }

  constexpr u8 offsets[13] = {1,  3,  5,  7,  9,  14, 16,
                              18, 20, 22, 24, 28, 30};
  const u32 base = static_cast<u32>(ordinal - 1) * 13;
  for (u32 index = 0; index < 13; ++index) {
    state.characters[base + index] = little_u16(entry + offsets[index]);
  }
  --state.expected;
}

}  // namespace detail

template <ReadableDevice Device>
class Volume {
 public:
  constexpr Volume() = default;

  [[nodiscard]] static Result<Volume> mount(Device& device) {
    Volume volume;
    volume.device_ = &device;
    const Error read_error =
        read_exact(device, 0, volume.sector_, sizeof(volume.sector_));
    if (read_error != Error::none) {
      return Result<Volume>::failure(read_error);
    }

    const u8* boot = volume.sector_;
    if (boot[510] != 0x55 || boot[511] != 0xaa) {
      return Result<Volume>::failure(Error::invalid_format);
    }
    auto& geometry = volume.geometry_;
    geometry.bytes_per_sector = little_u16(boot + 11);
    geometry.sectors_per_cluster = boot[13];
    geometry.reserved_sectors = little_u16(boot + 14);
    geometry.fat_count = boot[16];
    const u32 root_entry_count = little_u16(boot + 17);
    const u32 total_sectors_16 = little_u16(boot + 19);
    const u32 fat_sectors_16 = little_u16(boot + 22);
    geometry.total_sectors = little_u32(boot + 32);
    geometry.fat_sectors = little_u32(boot + 36);
    const u32 extended_flags = little_u16(boot + 40);
    const u32 version = little_u16(boot + 42);
    geometry.root_cluster = little_u32(boot + 44) & detail::fat_mask;
    geometry.fs_info_sector = little_u16(boot + 48);

    if ((geometry.bytes_per_sector != 512 &&
         geometry.bytes_per_sector != 1024 &&
         geometry.bytes_per_sector != 2048 &&
         geometry.bytes_per_sector != 4096) ||
        !power_of_two(geometry.sectors_per_cluster) ||
        geometry.sectors_per_cluster > 128 ||
        geometry.reserved_sectors == 0 || geometry.fat_count == 0 ||
        root_entry_count != 0 || total_sectors_16 != 0 ||
        fat_sectors_16 != 0 || geometry.total_sectors == 0 ||
        geometry.fat_sectors == 0 || version != 0) {
      return Result<Volume>::failure(Error::invalid_format);
    }
    geometry.bytes_per_cluster =
        geometry.bytes_per_sector * geometry.sectors_per_cluster;
    if (geometry.bytes_per_cluster > 32 * 1024) {
      return Result<Volume>::failure(Error::unsupported);
    }

    geometry.active_fat =
        (extended_flags & 0x80) != 0 ? extended_flags & 0x0f : 0;
    geometry.mirror_fats = (extended_flags & 0x80) == 0;
    if (geometry.active_fat >= geometry.fat_count) {
      return Result<Volume>::failure(Error::invalid_format);
    }

    const u64 fat_region =
        static_cast<u64>(geometry.fat_count) * geometry.fat_sectors;
    const u64 metadata_sectors = geometry.reserved_sectors + fat_region;
    if (metadata_sectors >= geometry.total_sectors) {
      return Result<Volume>::failure(Error::invalid_format);
    }
    geometry.first_data_sector = static_cast<u32>(metadata_sectors);
    const u32 data_sectors =
        geometry.total_sectors - geometry.first_data_sector;
    geometry.cluster_count =
        data_sectors / geometry.sectors_per_cluster;
    if (geometry.cluster_count < 65525 ||
        geometry.cluster_count > detail::reserved_cluster - 2) {
      return Result<Volume>::failure(Error::invalid_format);
    }
    const u32 maximum_cluster = geometry.cluster_count + 1;
    if (geometry.root_cluster < 2 ||
        geometry.root_cluster > maximum_cluster) {
      return Result<Volume>::failure(Error::invalid_format);
    }

    const u64 fat_bytes =
        static_cast<u64>(geometry.fat_sectors) *
        geometry.bytes_per_sector;
    const u64 required_fat_bytes =
        static_cast<u64>(geometry.cluster_count + 2) * 4;
    const u64 volume_bytes =
        static_cast<u64>(geometry.total_sectors) *
        geometry.bytes_per_sector;
    if (fat_bytes < required_fat_bytes ||
        volume_bytes > static_cast<u64>(device.size())) {
      return Result<Volume>::failure(Error::out_of_bounds);
    }

    if (geometry.fs_info_sector != 0xffff &&
        geometry.fs_info_sector < geometry.reserved_sectors) {
      const u64 fs_info_offset =
          static_cast<u64>(geometry.fs_info_sector) *
          geometry.bytes_per_sector;
      if (read_exact(device, fs_info_offset, volume.sector_,
                     geometry.bytes_per_sector) == Error::none &&
          little_u32(volume.sector_) == 0x41615252 &&
          little_u32(volume.sector_ + 484) == 0x61417272 &&
          little_u32(volume.sector_ + 508) == 0xaa550000) {
        const u32 free_count = little_u32(volume.sector_ + 488);
        const u32 next_free = little_u32(volume.sector_ + 492);
        if (free_count <= geometry.cluster_count) {
          volume.free_clusters_ = free_count;
          volume.free_clusters_known_ = true;
        }
        if (next_free >= 2 && next_free <= geometry.cluster_count + 1) {
          volume.allocation_hint_ = next_free;
        }
        volume.fs_info_valid_ = true;
      }
    }

    volume.mounted_ = true;
    return Result<Volume>::success(volume);
  }

  [[nodiscard]] constexpr const Geometry& geometry() const {
    return geometry_;
  }

  [[nodiscard]] constexpr Node root() const {
    return Node{geometry_.root_cluster, 0, Type::directory,
                detail::attribute_directory, 0};
  }

  template <typename Visitor>
  [[nodiscard]] Error for_each(const Node& directory, Visitor&& visitor) {
    if (!mounted_) {
      return Error::invalid_argument;
    }
    if (!directory.directory()) {
      return Error::not_directory;
    }
    if (!valid_cluster(directory.first_cluster)) {
      return Error::corrupt;
    }

    detail::LongNameState long_name;
    long_name.reset();
    u32 cluster = directory.first_cluster;
    for (u32 traversed = 0; traversed < geometry_.cluster_count;
         ++traversed) {
      const u64 cluster_offset = cluster_byte_offset(cluster);
      for (u32 sector = 0; sector < geometry_.sectors_per_cluster;
           ++sector) {
        const Error error = read_exact(
            *device_,
            cluster_offset +
                static_cast<u64>(sector) * geometry_.bytes_per_sector,
            sector_, geometry_.bytes_per_sector);
        if (error != Error::none) {
          return error;
        }
        for (u32 offset = 0; offset < geometry_.bytes_per_sector;
             offset += 32) {
          const u8* raw = sector_ + offset;
          if (raw[0] == 0) {
            return Error::none;
          }
          if (raw[0] == 0xe5) {
            long_name.reset();
            continue;
          }
          if (raw[11] == detail::attribute_long_name) {
            detail::consume_long_entry(raw, long_name);
            continue;
          }

          Entry entry;
          detail::decode_short_name(raw, entry.short_name);
          if (long_name.active && long_name.expected == 0 &&
              long_name.checksum == detail::short_checksum(raw) &&
              detail::decode_long_name(long_name, entry.name)) {
          } else {
            entry.name = entry.short_name;
          }
          long_name.reset();

          if ((raw[11] & detail::attribute_volume) != 0) {
            continue;
          }
          entry.node.attributes = raw[11];
          entry.node.type =
              (raw[11] & detail::attribute_directory) != 0
                  ? Type::directory
                  : Type::file;
          entry.node.first_cluster =
              ((static_cast<u32>(little_u16(raw + 20)) << 16) |
               little_u16(raw + 26)) &
              detail::fat_mask;
          entry.node.size = little_u32(raw + 28);
          entry.node.entry_offset =
              cluster_offset +
              static_cast<u64>(sector) * geometry_.bytes_per_sector +
              offset;
          if ((entry.node.first_cluster != 0 &&
               !valid_cluster(entry.node.first_cluster)) ||
              (entry.node.directory() &&
               entry.node.first_cluster == 0 &&
               !entry.name.equals(".."))) {
            return Error::corrupt;
          }
          if (!visitor(entry)) {
            return Error::none;
          }
        }
      }

      const auto next = next_cluster(cluster);
      if (!next) {
        return next.error;
      }
      if (next.value.end) {
        return Error::none;
      }
      cluster = next.value.cluster;
    }
    return Error::loop;
  }

  [[nodiscard]] Result<Node> lookup(const Node& directory,
                                    const char* name) {
    if (name == nullptr || *name == '\0') {
      return Result<Node>::failure(Error::invalid_argument);
    }
    Node found{};
    bool matched = false;
    const Error error = for_each(directory, [&](const Entry& entry) {
      if (entry.name.equals(name, true) ||
          entry.short_name.equals(name, true)) {
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
    Node current = root();
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
      if (size == 2 && component[0] == '.' && component[1] == '.') {
        return Result<Node>::failure(Error::unsupported);
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
    if (offset >= file.size || size == 0) {
      return Result<u32>::success(0);
    }
    u32 remaining = size;
    const u64 available = static_cast<u64>(file.size) - offset;
    if (remaining > available) {
      remaining = static_cast<u32>(available);
    }
    if (remaining != 0 && !valid_cluster(file.first_cluster)) {
      return Result<u32>::failure(Error::corrupt);
    }

    u32 cluster = file.first_cluster;
    u64 clusters_to_skip = offset / geometry_.bytes_per_cluster;
    u32 traversed = 0;
    while (clusters_to_skip-- != 0) {
      if (++traversed > geometry_.cluster_count) {
        return Result<u32>::failure(Error::loop);
      }
      const auto next = next_cluster(cluster);
      if (!next) {
        return Result<u32>::failure(next.error);
      }
      if (next.value.end) {
        return Result<u32>::failure(Error::corrupt);
      }
      cluster = next.value.cluster;
    }

    u32 within = static_cast<u32>(offset % geometry_.bytes_per_cluster);
    const u32 requested = remaining;
    while (remaining != 0) {
      const u32 available_in_cluster =
          geometry_.bytes_per_cluster - within;
      const u32 count =
          remaining < available_in_cluster ? remaining
                                           : available_in_cluster;
      const Error error =
          read_exact(*device_, cluster_byte_offset(cluster) + within,
                     output, count);
      if (error != Error::none) {
        return Result<u32>::failure(error);
      }
      output += count;
      remaining -= count;
      within = 0;
      if (remaining == 0) {
        break;
      }
      if (++traversed > geometry_.cluster_count) {
        return Result<u32>::failure(Error::loop);
      }
      const auto next = next_cluster(cluster);
      if (!next) {
        return Result<u32>::failure(next.error);
      }
      if (next.value.end) {
        return Result<u32>::failure(Error::corrupt);
      }
      cluster = next.value.cluster;
    }
    return Result<u32>::success(requested);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error flush() {
    return device_->flush() ? Error::none : Error::io;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error truncate(Node& file, u32 new_size) {
    if (!mounted_) {
      return Error::invalid_argument;
    }
    if (file.directory()) {
      return Error::is_directory;
    }
    if (file.entry_offset == 0 && new_size != file.size) {
      return Error::invalid_argument;
    }

    const u32 old_clusters = clusters_for_size(file.size);
    const u32 new_clusters = clusters_for_size(new_size);
    if (old_clusters == new_clusters) {
      if (new_size > file.size) {
        const Error zero_error =
            zero_file_range(file.first_cluster, file.size,
                            new_size - file.size);
        if (zero_error != Error::none) {
          return zero_error;
        }
      }
      const Error metadata = write_node_metadata(file, file.first_cluster,
                                                 new_size);
      if (metadata != Error::none) {
        return metadata;
      }
      file.size = new_size;
      return flush();
    }

    if (new_clusters == 0) {
      const u32 old_first = file.first_cluster;
      Error error = write_node_metadata(file, 0, 0);
      if (error != Error::none || (error = flush()) != Error::none) {
        return error;
      }
      file.first_cluster = 0;
      file.size = 0;
      error = free_chain(old_first);
      if (error != Error::none) {
        return error;
      }
      return commit_fs_info();
    }

    if (old_clusters == 0) {
      u32 first = 0;
      u32 last = 0;
      Error error = allocate_chain(new_clusters, first, last);
      if (error != Error::none) {
        return error;
      }
      error = flush();
      if (error == Error::none) {
        error = write_node_metadata(file, first, new_size);
      }
      if (error == Error::none) {
        error = flush();
      }
      if (error != Error::none) {
        (void)free_chain(first);
        return error;
      }
      file.first_cluster = first;
      file.size = new_size;
      return commit_fs_info();
    }

    u32 last_kept = file.first_cluster;
    for (u32 index = 1; index <
         (old_clusters < new_clusters ? old_clusters : new_clusters);
         ++index) {
      const auto next = next_cluster(last_kept);
      if (!next || next.value.end) {
        return !next ? next.error : Error::corrupt;
      }
      last_kept = next.value.cluster;
    }

    if (new_clusters > old_clusters) {
      u32 added_first = 0;
      u32 added_last = 0;
      Error error = allocate_chain(new_clusters - old_clusters,
                                   added_first, added_last);
      if (error != Error::none) {
        return error;
      }
      error = flush();
      if (error == Error::none) {
        error = write_fat_entry(last_kept, added_first);
      }
      if (error == Error::none) {
        error = flush();
      }
      if (error == Error::none) {
        error = zero_file_range(file.first_cluster, file.size,
                                new_size - file.size);
      }
      if (error == Error::none) {
        error = write_node_metadata(file, file.first_cluster, new_size);
      }
      if (error == Error::none) {
        error = flush();
      }
      if (error != Error::none) {
        (void)write_fat_entry(last_kept, detail::end_of_chain);
        (void)free_chain(added_first);
        return error;
      }
    } else {
      const auto tail = next_cluster(last_kept);
      if (!tail || tail.value.end) {
        return !tail ? tail.error : Error::corrupt;
      }
      Error error = write_fat_entry(last_kept, detail::end_of_chain);
      if (error == Error::none) {
        error = write_node_metadata(file, file.first_cluster, new_size);
      }
      if (error == Error::none) {
        error = flush();
      }
      if (error != Error::none) {
        return error;
      }
      error = free_chain(tail.value.cluster);
      if (error != Error::none) {
        return error;
      }
    }

    file.size = new_size;
    return commit_fs_info();
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Result<u32> write(Node& file, u64 offset,
                                  const u8* input, u32 size) {
    if (!mounted_ || (input == nullptr && size != 0) ||
        offset > 0xffffffffu || size > 0xffffffffu - offset) {
      return Result<u32>::failure(Error::invalid_argument);
    }
    if (file.directory()) {
      return Result<u32>::failure(Error::is_directory);
    }
    if (size == 0) {
      return Result<u32>::success(0);
    }
    const u32 end = static_cast<u32>(offset) + size;
    if (end > file.size) {
      const Error error = truncate(file, end);
      if (error != Error::none) {
        return Result<u32>::failure(error);
      }
    }

    u32 cluster = file.first_cluster;
    u32 skip = static_cast<u32>(offset) / geometry_.bytes_per_cluster;
    for (u32 traversed = 0; traversed < skip; ++traversed) {
      const auto next = next_cluster(cluster);
      if (!next || next.value.end) {
        return Result<u32>::failure(!next ? next.error : Error::corrupt);
      }
      cluster = next.value.cluster;
    }
    u32 within = static_cast<u32>(offset) % geometry_.bytes_per_cluster;
    u32 remaining = size;
    while (remaining != 0) {
      const u32 count =
          remaining < geometry_.bytes_per_cluster - within
              ? remaining
              : geometry_.bytes_per_cluster - within;
      const Error error = write_exact(*device_,
                                      cluster_byte_offset(cluster) + within,
                                      input, count);
      if (error != Error::none) {
        return Result<u32>::failure(error);
      }
      input += count;
      remaining -= count;
      within = 0;
      if (remaining != 0) {
        const auto next = next_cluster(cluster);
        if (!next || next.value.end) {
          return Result<u32>::failure(!next ? next.error : Error::corrupt);
        }
        cluster = next.value.cluster;
      }
    }
    const Error error = flush();
    return error == Error::none ? Result<u32>::success(size)
                                : Result<u32>::failure(error);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error create(const char* path, const u8* input, u32 size) {
    if ((input == nullptr && size != 0) || path == nullptr) {
      return Error::invalid_argument;
    }
    Node parent{};
    char leaf[Name::capacity + 1]{};
    Error error = resolve_parent(path, parent, leaf);
    if (error != Error::none) {
      return error;
    }
    const auto existing = lookup(parent, leaf);
    if (existing) {
      return Error::already_exists;
    }
    if (existing.error != Error::not_found) {
      return existing.error;
    }
    Node file{};
    file.attributes = 0x20;
    error = insert_directory_record(parent, leaf, file);
    if (error != Error::none) {
      return error;
    }
    if (size != 0) {
      const auto written = write(file, 0, input, size);
      if (!written) {
        (void)erase_directory_record(parent, file.entry_offset);
        if (file.first_cluster != 0) {
          (void)free_chain(file.first_cluster);
        }
        return written.error;
      }
    }
    return Error::none;
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
    return file.value.directory()
               ? Result<u32>::failure(Error::is_directory)
               : Result<u32>::success(file.value.size);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error remove(const char* path) {
    Node parent{};
    char leaf[Name::capacity + 1]{};
    Error error = resolve_parent(path, parent, leaf);
    if (error != Error::none) {
      return error;
    }
    const auto found = lookup(parent, leaf);
    if (!found) {
      return found.error;
    }
    if (found.value.directory()) {
      bool nonempty = false;
      error = for_each(found.value, [&](const Entry& entry) {
        if (!entry.name.equals(".") && !entry.name.equals("..")) {
          nonempty = true;
          return false;
        }
        return true;
      });
      if (error != Error::none) {
        return error;
      }
      if (nonempty) {
        return Error::unsupported;
      }
    }
    error = erase_directory_record(parent, found.value.entry_offset);
    if (error != Error::none || (error = flush()) != Error::none) {
      return error;
    }
    error = free_chain(found.value.first_cluster);
    return error == Error::none ? commit_fs_info() : error;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error move(const char* source, const char* destination) {
    Node source_parent{};
    Node destination_parent{};
    char source_leaf[Name::capacity + 1]{};
    char destination_leaf[Name::capacity + 1]{};
    Error error = resolve_parent(source, source_parent, source_leaf);
    if (error != Error::none) {
      return error;
    }
    error = resolve_parent(destination, destination_parent,
                           destination_leaf);
    if (error != Error::none) {
      return error;
    }
    const auto source_node = lookup(source_parent, source_leaf);
    if (!source_node) {
      return source_node.error;
    }
    const auto destination_node =
        lookup(destination_parent, destination_leaf);
    if (destination_node) {
      return Error::already_exists;
    }
    if (destination_node.error != Error::not_found) {
      return destination_node.error;
    }
    Node replacement = source_node.value;
    replacement.entry_offset = 0;
    error = insert_directory_record(destination_parent, destination_leaf,
                                    replacement);
    if (error != Error::none) {
      return error;
    }
    error = erase_directory_record(source_parent,
                                   source_node.value.entry_offset);
    return error == Error::none ? flush() : error;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error concatenate(const char* destination,
                                  const char* source) {
    auto destination_node = lookup_path(destination);
    const auto source_node = lookup_path(source);
    if (!destination_node) {
      return destination_node.error;
    }
    if (!source_node) {
      return source_node.error;
    }
    if (destination_node.value.directory() ||
        source_node.value.directory()) {
      return Error::is_directory;
    }
    if (destination_node.value.size > 0xffffffffu ||
        source_node.value.size >
            0xffffffffu - destination_node.value.size) {
      return Error::no_space;
    }
    const u32 destination_size =
        static_cast<u32>(destination_node.value.size);
    const u32 source_size = static_cast<u32>(source_node.value.size);
    Error error = truncate(destination_node.value,
                           destination_size + source_size);
    if (error != Error::none) {
      return error;
    }
    u32 copied = 0;
    while (copied < source_size) {
      const u32 count = source_size - copied < geometry_.bytes_per_sector
                            ? source_size - copied
                            : geometry_.bytes_per_sector;
      const auto loaded = read(source_node.value, copied, sector_, count);
      if (!loaded || loaded.value != count) {
        return !loaded ? loaded.error : Error::io;
      }
      const auto stored = write(destination_node.value,
                                destination_size + copied, sector_, count);
      if (!stored || stored.value != count) {
        return !stored ? stored.error : Error::io;
      }
      copied += count;
    }
    return Error::none;
  }

  [[nodiscard]] bool consistent() {
    if (!mounted_ || !valid_cluster(geometry_.root_cluster)) {
      return false;
    }
    const auto root_link = fat_entry(geometry_.root_cluster);
    if (!root_link || root_link.value == 0 ||
        root_link.value == detail::bad_cluster) {
      return false;
    }
    return for_each(root(), [](const Entry&) { return true; }) ==
           Error::none;
  }

 private:
  struct ClusterLink {
    u32 cluster{};
    bool end{};
  };

  [[nodiscard]] Error resolve_parent(const char* path, Node& parent,
                                     char* leaf) {
    if (!mounted_ || path == nullptr || leaf == nullptr) {
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
    if (length == 0) {
      return Error::invalid_argument;
    }
    u32 leaf_start = length;
    while (leaf_start != 0 && path[leaf_start - 1] != '/') {
      --leaf_start;
    }
    const u32 leaf_size = length - leaf_start;
    if (leaf_size == 0 || leaf_size > 255 ||
        (leaf_size == 1 && path[leaf_start] == '.') ||
        (leaf_size == 2 && path[leaf_start] == '.' &&
         path[leaf_start + 1] == '.')) {
      return Error::invalid_argument;
    }
    for (u32 index = 0; index < leaf_size; ++index) {
      leaf[index] = path[leaf_start + index];
    }
    leaf[leaf_size] = '\0';
    if (leaf_start == 0 ||
        (leaf_start == 1 && path[0] == '/')) {
      parent = root();
      return Error::none;
    }
    char parent_path[Name::capacity + 1]{};
    u32 parent_size = leaf_start;
    while (parent_size > 1 && path[parent_size - 1] == '/') {
      --parent_size;
    }
    for (u32 index = 0; index < parent_size; ++index) {
      parent_path[index] = path[index];
    }
    parent_path[parent_size] = '\0';
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

  [[nodiscard]] static bool forbidden_name_byte(u8 value) {
    return value < 0x20 || value == '"' || value == '*' || value == '/' ||
           value == ':' || value == '<' || value == '>' || value == '?' ||
           value == '\\' || value == '|';
  }

  [[nodiscard]] static bool decode_utf8_name(const char* name, u16* output,
                                             u32& output_size) {
    output_size = 0;
    if (name == nullptr || *name == '\0') {
      return false;
    }
    for (u32 cursor = 0; name[cursor] != '\0';) {
      const u8 first = static_cast<u8>(name[cursor++]);
      if (forbidden_name_byte(first)) {
        return false;
      }
      u32 codepoint = 0;
      u32 continuation = 0;
      if (first < 0x80) {
        codepoint = first;
      } else if ((first & 0xe0) == 0xc0) {
        codepoint = first & 0x1f;
        continuation = 1;
      } else if ((first & 0xf0) == 0xe0) {
        codepoint = first & 0x0f;
        continuation = 2;
      } else if ((first & 0xf8) == 0xf0) {
        codepoint = first & 0x07;
        continuation = 3;
      } else {
        return false;
      }
      for (u32 index = 0; index < continuation; ++index) {
        const u8 byte = static_cast<u8>(name[cursor++]);
        if ((byte & 0xc0) != 0x80) {
          return false;
        }
        codepoint = (codepoint << 6) | (byte & 0x3f);
      }
      if ((continuation == 1 && codepoint < 0x80) ||
          (continuation == 2 && codepoint < 0x800) ||
          (continuation == 3 && codepoint < 0x10000) ||
          codepoint > 0x10ffff ||
          (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return false;
      }
      if (codepoint <= 0xffff) {
        if (output_size == 255) {
          return false;
        }
        output[output_size++] = static_cast<u16>(codepoint);
      } else {
        if (output_size > 253) {
          return false;
        }
        codepoint -= 0x10000;
        output[output_size++] =
            static_cast<u16>(0xd800 + (codepoint >> 10));
        output[output_size++] =
            static_cast<u16>(0xdc00 + (codepoint & 0x3ff));
      }
    }
    return output_size != 0 && output[output_size - 1] != ' ' &&
           output[output_size - 1] != '.';
  }

  [[nodiscard]] Error make_short_alias(const Node& directory,
                                       const char* name, u8* alias) {
    char base[7]{};
    char extension[4]{};
    u32 base_size = 0;
    u32 extension_size = 0;
    u32 dot = 0xffffffffu;
    u32 length = 0;
    while (name[length] != '\0') {
      if (name[length] == '.') {
        dot = length;
      }
      ++length;
    }
    const u32 base_end = dot == 0xffffffffu ? length : dot;
    for (u32 index = 0; index < base_end && base_size < 6; ++index) {
      u8 value = static_cast<u8>(name[index]);
      if (value >= 'a' && value <= 'z') {
        value = static_cast<u8>(value - ('a' - 'A'));
      }
      if ((value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '_') {
        base[base_size++] = static_cast<char>(value);
      }
    }
    if (base_size == 0) {
      base[base_size++] = '_';
    }
    if (dot != 0xffffffffu) {
      for (u32 index = dot + 1; index < length && extension_size < 3;
           ++index) {
        u8 value = static_cast<u8>(name[index]);
        if (value >= 'a' && value <= 'z') {
          value = static_cast<u8>(value - ('a' - 'A'));
        }
        if ((value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_') {
          extension[extension_size++] = static_cast<char>(value);
        }
      }
    }
    for (u32 sequence = 1; sequence < 100; ++sequence) {
      for (u32 index = 0; index < 11; ++index) {
        alias[index] = ' ';
      }
      for (u32 index = 0; index < base_size; ++index) {
        alias[index] = static_cast<u8>(base[index]);
      }
      alias[base_size] = '~';
      if (sequence < 10) {
        alias[base_size + 1] = static_cast<u8>('0' + sequence);
      } else {
        alias[base_size + 1] = static_cast<u8>('0' + sequence / 10);
        alias[base_size + 2] = static_cast<u8>('0' + sequence % 10);
      }
      for (u32 index = 0; index < extension_size; ++index) {
        alias[8 + index] = static_cast<u8>(extension[index]);
      }
      bool collision = false;
      const Error error = for_each(directory, [&](const Entry& entry) {
        u8 existing[11]{};
        u32 base_index = 0;
        while (base_index < entry.short_name.size &&
               entry.short_name.data[base_index] != '.' && base_index < 8) {
          u8 value = static_cast<u8>(entry.short_name.data[base_index]);
          if (value >= 'a' && value <= 'z') {
            value = static_cast<u8>(value - ('a' - 'A'));
          }
          existing[base_index++] = value;
        }
        for (; base_index < 8; ++base_index) {
          existing[base_index] = ' ';
        }
        u32 cursor = 0;
        while (cursor < entry.short_name.size &&
               entry.short_name.data[cursor] != '.') {
          ++cursor;
        }
        if (cursor < entry.short_name.size) {
          ++cursor;
        }
        for (u32 index = 0; index < 3; ++index) {
          existing[8 + index] =
              cursor + index < entry.short_name.size
                  ? static_cast<u8>(entry.short_name.data[cursor + index])
                  : static_cast<u8>(' ');
        }
        collision = true;
        for (u32 index = 0; index < 11; ++index) {
          if (existing[index] != alias[index]) {
            collision = false;
            break;
          }
        }
        return !collision;
      });
      if (error != Error::none) {
        return error;
      }
      if (!collision) {
        return Error::none;
      }
    }
    return Error::no_space;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error find_free_directory_slots(const Node& directory,
                                                u32 needed,
                                                u64* offsets) {
    if (!directory.directory() || needed == 0 || needed > 21) {
      return Error::invalid_argument;
    }
    u32 found = 0;
    u32 cluster = directory.first_cluster;
    for (u32 traversed = 0; traversed < geometry_.cluster_count;
         ++traversed) {
      const u64 base = cluster_byte_offset(cluster);
      for (u32 slot = 0; slot < geometry_.bytes_per_cluster / 32; ++slot) {
        const u64 offset = base + static_cast<u64>(slot) * 32;
        u8 marker = 0;
        const Error error = read_exact(*device_, offset, &marker, 1);
        if (error != Error::none) {
          return error;
        }
        if (marker == 0 || marker == 0xe5) {
          offsets[found++] = offset;
          if (found == needed) {
            return Error::none;
          }
        } else {
          found = 0;
        }
      }
      const auto next = next_cluster(cluster);
      if (!next) {
        return next.error;
      }
      if (!next.value.end) {
        cluster = next.value.cluster;
        continue;
      }

      const auto added = allocate_cluster();
      if (!added) {
        return added.error;
      }
      Error error = flush();
      if (error == Error::none) {
        error = write_fat_entry(cluster, added.value);
      }
      if (error == Error::none) {
        error = flush();
      }
      if (error != Error::none) {
        (void)write_fat_entry(added.value, 0);
        return error;
      }
      cluster = added.value;
    }
    return Error::loop;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error insert_directory_record(const Node& directory,
                                              const char* name, Node& node) {
    u16 utf16[255]{};
    u32 utf16_size = 0;
    if (!decode_utf8_name(name, utf16, utf16_size)) {
      return Error::invalid_argument;
    }
    u8 alias[11]{};
    Error error = make_short_alias(directory, name, alias);
    if (error != Error::none) {
      return error;
    }
    const u32 parts = (utf16_size + 12) / 13;
    u64 offsets[21]{};
    error = find_free_directory_slots(directory, parts + 1, offsets);
    if (error != Error::none) {
      return error;
    }

    constexpr u8 character_offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
    };
    const u8 checksum = detail::short_checksum(alias);
    for (u32 disk_part = 0; disk_part < parts; ++disk_part) {
      const u32 ordinal = parts - disk_part;
      u8 raw[32]{};
      for (u32 index = 0; index < sizeof(raw); ++index) {
        raw[index] = 0xff;
      }
      raw[0] = static_cast<u8>(ordinal |
                               (ordinal == parts ? 0x40 : 0));
      raw[11] = detail::attribute_long_name;
      raw[12] = 0;
      raw[13] = checksum;
      raw[26] = 0;
      raw[27] = 0;
      const u32 base = (ordinal - 1) * 13;
      for (u32 index = 0; index < 13; ++index) {
        u16 value = 0xffff;
        if (base + index < utf16_size) {
          value = utf16[base + index];
        } else if (base + index == utf16_size) {
          value = 0;
        }
        raw[character_offsets[index]] = static_cast<u8>(value);
        raw[character_offsets[index] + 1] = static_cast<u8>(value >> 8);
      }
      error = write_exact(*device_, offsets[disk_part], raw, sizeof(raw));
      if (error != Error::none) {
        return error;
      }
    }
    error = flush();
    if (error != Error::none) {
      return error;
    }

    u8 raw[32]{};
    for (u32 index = 0; index < 11; ++index) {
      raw[index] = alias[index];
    }
    raw[11] = node.attributes;
    raw[20] = static_cast<u8>(node.first_cluster >> 16);
    raw[21] = static_cast<u8>(node.first_cluster >> 24);
    raw[26] = static_cast<u8>(node.first_cluster);
    raw[27] = static_cast<u8>(node.first_cluster >> 8);
    raw[28] = static_cast<u8>(node.size);
    raw[29] = static_cast<u8>(node.size >> 8);
    raw[30] = static_cast<u8>(node.size >> 16);
    raw[31] = static_cast<u8>(node.size >> 24);
    error = write_exact(*device_, offsets[parts], raw, sizeof(raw));
    if (error != Error::none) {
      return error;
    }
    node.entry_offset = offsets[parts];
    return flush();
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error erase_directory_record(const Node& directory,
                                             u64 entry_offset) {
    if (!directory.directory() || entry_offset == 0) {
      return Error::invalid_argument;
    }
    u64 long_offsets[20]{};
    u32 long_count = 0;
    u32 cluster = directory.first_cluster;
    for (u32 traversed = 0; traversed < geometry_.cluster_count;
         ++traversed) {
      const u64 base = cluster_byte_offset(cluster);
      for (u32 slot = 0; slot < geometry_.bytes_per_cluster / 32; ++slot) {
        const u64 offset = base + static_cast<u64>(slot) * 32;
        u8 header[14]{};
        Error error = read_exact(*device_, offset, header, sizeof(header));
        if (error != Error::none) {
          return error;
        }
        if (header[0] == 0) {
          return Error::not_found;
        }
        if (header[0] == 0xe5) {
          long_count = 0;
          continue;
        }
        if (header[11] == detail::attribute_long_name) {
          if (long_count < 20) {
            long_offsets[long_count++] = offset;
          } else {
            long_count = 0;
          }
          continue;
        }
        if (offset == entry_offset) {
          u8 deleted = 0xe5;
          for (u32 index = 0; index < long_count; ++index) {
            error = write_exact(*device_, long_offsets[index], &deleted, 1);
            if (error != Error::none) {
              return error;
            }
          }
          return write_exact(*device_, offset, &deleted, 1);
        }
        long_count = 0;
      }
      const auto next = next_cluster(cluster);
      if (!next) {
        return next.error;
      }
      if (next.value.end) {
        return Error::not_found;
      }
      cluster = next.value.cluster;
    }
    return Error::loop;
  }

  [[nodiscard]] constexpr bool valid_cluster(u32 cluster) const {
    return cluster >= 2 && cluster <= geometry_.cluster_count + 1;
  }

  [[nodiscard]] constexpr u64 cluster_byte_offset(u32 cluster) const {
    const u64 sector =
        geometry_.first_data_sector +
        static_cast<u64>(cluster - 2) *
            geometry_.sectors_per_cluster;
    return sector * geometry_.bytes_per_sector;
  }

  [[nodiscard]] constexpr u32 clusters_for_size(u32 size) const {
    return size == 0
               ? 0
               : 1 + (size - 1) / geometry_.bytes_per_cluster;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error zero_file_range(u32 first_cluster, u32 offset,
                                      u32 size) {
    if (size == 0) {
      return Error::none;
    }
    if (!valid_cluster(first_cluster)) {
      return Error::corrupt;
    }
    zero_bytes(sector_, geometry_.bytes_per_sector);
    u32 cluster = first_cluster;
    u32 skip = offset / geometry_.bytes_per_cluster;
    for (u32 index = 0; index < skip; ++index) {
      const auto next = next_cluster(cluster);
      if (!next || next.value.end) {
        return !next ? next.error : Error::corrupt;
      }
      cluster = next.value.cluster;
    }
    u32 within = offset % geometry_.bytes_per_cluster;
    u32 remaining = size;
    while (remaining != 0) {
      u32 cluster_count = geometry_.bytes_per_cluster - within;
      if (cluster_count > remaining) {
        cluster_count = remaining;
      }
      u64 target = cluster_byte_offset(cluster) + within;
      while (cluster_count != 0) {
        const u32 count = cluster_count < geometry_.bytes_per_sector
                              ? cluster_count
                              : geometry_.bytes_per_sector;
        const Error error =
            write_exact(*device_, target, sector_, count);
        if (error != Error::none) {
          return error;
        }
        target += count;
        cluster_count -= count;
        remaining -= count;
      }
      within = 0;
      if (remaining != 0) {
        const auto next = next_cluster(cluster);
        if (!next || next.value.end) {
          return !next ? next.error : Error::corrupt;
        }
        cluster = next.value.cluster;
      }
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error write_node_metadata(const Node& node,
                                          u32 first_cluster, u32 size) {
    if (node.entry_offset == 0) {
      return Error::invalid_argument;
    }
    u8 raw[12]{};
    const Error read_error =
        read_exact(*device_, node.entry_offset + 20, raw, sizeof(raw));
    if (read_error != Error::none) {
      return read_error;
    }
    raw[0] = static_cast<u8>(first_cluster >> 16);
    raw[1] = static_cast<u8>(first_cluster >> 24);
    raw[6] = static_cast<u8>(first_cluster);
    raw[7] = static_cast<u8>(first_cluster >> 8);
    raw[8] = static_cast<u8>(size);
    raw[9] = static_cast<u8>(size >> 8);
    raw[10] = static_cast<u8>(size >> 16);
    raw[11] = static_cast<u8>(size >> 24);
    return write_exact(*device_, node.entry_offset + 20, raw, sizeof(raw));
  }

  [[nodiscard]] Result<u32> fat_entry(u32 cluster) {
    if (!valid_cluster(cluster)) {
      return Result<u32>::failure(Error::corrupt);
    }
    const u64 fat_sector =
        geometry_.reserved_sectors +
        static_cast<u64>(geometry_.active_fat) * geometry_.fat_sectors;
    u8 raw[4]{};
    const Error error = read_exact(
        *device_, fat_sector * geometry_.bytes_per_sector +
                      static_cast<u64>(cluster) * 4,
        raw, sizeof(raw));
    return error == Error::none
               ? Result<u32>::success(little_u32(raw) & detail::fat_mask)
               : Result<u32>::failure(error);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error write_fat_entry(u32 cluster, u32 value) {
    if (!valid_cluster(cluster)) {
      return Error::corrupt;
    }
    const u32 first_fat = geometry_.mirror_fats ? 0 : geometry_.active_fat;
    const u32 fat_limit = geometry_.mirror_fats
                              ? geometry_.fat_count
                              : geometry_.active_fat + 1;
    for (u32 fat = first_fat; fat < fat_limit; ++fat) {
      const u64 offset =
          (geometry_.reserved_sectors +
           static_cast<u64>(fat) * geometry_.fat_sectors) *
              geometry_.bytes_per_sector +
          static_cast<u64>(cluster) * 4;
      u8 raw[4]{};
      Error error = read_exact(*device_, offset, raw, sizeof(raw));
      if (error != Error::none) {
        return error;
      }
      const u32 stored = (little_u32(raw) & 0xf0000000) |
                         (value & detail::fat_mask);
      raw[0] = static_cast<u8>(stored);
      raw[1] = static_cast<u8>(stored >> 8);
      raw[2] = static_cast<u8>(stored >> 16);
      raw[3] = static_cast<u8>(stored >> 24);
      error = write_exact(*device_, offset, raw, sizeof(raw));
      if (error != Error::none) {
        return error;
      }
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Result<u32> allocate_cluster() {
    u32 candidate = allocation_hint_;
    if (!valid_cluster(candidate)) {
      candidate = 2;
    }
    for (u32 scanned = 0; scanned < geometry_.cluster_count; ++scanned) {
      const auto value = fat_entry(candidate);
      if (!value) {
        return Result<u32>::failure(value.error);
      }
      if (value.value == 0) {
        Error error = write_fat_entry(candidate, detail::end_of_chain);
        if (error != Error::none) {
          return Result<u32>::failure(error);
        }
        zero_bytes(sector_, geometry_.bytes_per_sector);
        const u64 base = cluster_byte_offset(candidate);
        for (u32 sector = 0; sector < geometry_.sectors_per_cluster;
             ++sector) {
          error = write_exact(
              *device_,
              base + static_cast<u64>(sector) * geometry_.bytes_per_sector,
              sector_, geometry_.bytes_per_sector);
          if (error != Error::none) {
            (void)write_fat_entry(candidate, 0);
            return Result<u32>::failure(error);
          }
        }
        allocation_hint_ = candidate == geometry_.cluster_count + 1
                               ? 2
                               : candidate + 1;
        if (free_clusters_known_ && free_clusters_ != 0) {
          --free_clusters_;
        }
        return Result<u32>::success(candidate);
      }
      candidate = candidate == geometry_.cluster_count + 1
                      ? 2
                      : candidate + 1;
    }
    return Result<u32>::failure(Error::no_space);
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error allocate_chain(u32 count, u32& first, u32& last) {
    first = 0;
    last = 0;
    for (u32 index = 0; index < count; ++index) {
      const auto allocated = allocate_cluster();
      if (!allocated) {
        if (first != 0) {
          (void)free_chain(first);
        }
        return allocated.error;
      }
      if (first == 0) {
        first = allocated.value;
      } else {
        const Error error = write_fat_entry(last, allocated.value);
        if (error != Error::none) {
          (void)write_fat_entry(allocated.value, 0);
          (void)free_chain(first);
          return error;
        }
      }
      last = allocated.value;
    }
    return Error::none;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error free_chain(u32 first) {
    if (first == 0) {
      return Error::none;
    }
    u32 cluster = first;
    for (u32 traversed = 0; traversed < geometry_.cluster_count;
         ++traversed) {
      const auto value = fat_entry(cluster);
      if (!value) {
        return value.error;
      }
      const bool end = value.value >= detail::end_of_chain;
      const u32 next = value.value;
      const Error error = write_fat_entry(cluster, 0);
      if (error != Error::none) {
        return error;
      }
      if (free_clusters_known_ && free_clusters_ < geometry_.cluster_count) {
        ++free_clusters_;
      }
      if (cluster < allocation_hint_) {
        allocation_hint_ = cluster;
      }
      if (end) {
        return Error::none;
      }
      if (next == 0 || next == 1 || next == detail::bad_cluster ||
          next >= detail::reserved_cluster || !valid_cluster(next)) {
        return Error::corrupt;
      }
      cluster = next;
    }
    return Error::loop;
  }

  template <typename D = Device>
    requires WritableDevice<D>
  [[nodiscard]] Error commit_fs_info() {
    if (!fs_info_valid_) {
      return flush();
    }
    const u64 offset = static_cast<u64>(geometry_.fs_info_sector) *
                       geometry_.bytes_per_sector;
    Error error = read_exact(*device_, offset, sector_,
                             geometry_.bytes_per_sector);
    if (error != Error::none) {
      return error;
    }
    if (free_clusters_known_) {
      sector_[488] = static_cast<u8>(free_clusters_);
      sector_[489] = static_cast<u8>(free_clusters_ >> 8);
      sector_[490] = static_cast<u8>(free_clusters_ >> 16);
      sector_[491] = static_cast<u8>(free_clusters_ >> 24);
    }
    sector_[492] = static_cast<u8>(allocation_hint_);
    sector_[493] = static_cast<u8>(allocation_hint_ >> 8);
    sector_[494] = static_cast<u8>(allocation_hint_ >> 16);
    sector_[495] = static_cast<u8>(allocation_hint_ >> 24);
    error = write_exact(*device_, offset, sector_, geometry_.bytes_per_sector);
    if (error != Error::none) {
      return error;
    }
    return flush();
  }

  [[nodiscard]] Result<ClusterLink> next_cluster(u32 cluster) {
    if (!valid_cluster(cluster)) {
      return Result<ClusterLink>::failure(Error::corrupt);
    }
    const u64 fat_sector =
        geometry_.reserved_sectors +
        static_cast<u64>(geometry_.active_fat) *
            geometry_.fat_sectors;
    const u64 offset =
        fat_sector * geometry_.bytes_per_sector +
        static_cast<u64>(cluster) * 4;
    u8 raw[4]{};
    const Error error = read_exact(*device_, offset, raw, sizeof(raw));
    if (error != Error::none) {
      return Result<ClusterLink>::failure(error);
    }
    const u32 value = little_u32(raw) & detail::fat_mask;
    if (value >= detail::end_of_chain) {
      return Result<ClusterLink>::success(ClusterLink{0, true});
    }
    if (value == 0 || value == 1 || value == detail::bad_cluster ||
        value >= detail::reserved_cluster || !valid_cluster(value)) {
      return Result<ClusterLink>::failure(Error::corrupt);
    }
    return Result<ClusterLink>::success(ClusterLink{value, false});
  }

  Device* device_{};
  Geometry geometry_{};
  bool mounted_{};
  bool fs_info_valid_{};
  bool free_clusters_known_{};
  u32 free_clusters_{};
  u32 allocation_hint_{2};
  u8 sector_[4096]{};
};

}  // namespace mikos::drivers::fs::fat32
