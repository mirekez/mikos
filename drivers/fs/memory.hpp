#pragma once

#include <drivers/fs/filesystem.hpp>

namespace mikos::drivers::fs {

template <u32 MaxFiles, u32 MaxFileBytes, bool FoldAsciiCase = false,
          u32 MaxPathBytes = 127>
class MemoryFilesystem {
  struct File {
    char path[MaxPathBytes + 1]{};
    u8 data[MaxFileBytes]{};
    u32 size{};
    bool used{};
  };

 public:
  [[nodiscard]] Error create(const char* path, const u8* data, u32 size) {
    if (!valid_path(path) || (data == nullptr && size != 0)) {
      return Error::invalid_argument;
    }
    if (find(path) != MaxFiles) {
      return Error::already_exists;
    }
    if (size > MaxFileBytes) {
      return Error::no_space;
    }
    const u32 slot = unused();
    if (slot == MaxFiles) {
      return Error::no_space;
    }
    copy_path(files_[slot].path, path);
    copy(files_[slot].data, data, size);
    files_[slot].size = size;
    files_[slot].used = true;
    return Error::none;
  }

  [[nodiscard]] Error move(const char* source, const char* destination) {
    if (!valid_path(source) || !valid_path(destination)) {
      return Error::invalid_argument;
    }
    const u32 source_slot = find(source);
    if (source_slot == MaxFiles) {
      return Error::not_found;
    }
    if (find(destination) != MaxFiles) {
      return Error::already_exists;
    }
    copy_path(files_[source_slot].path, destination);
    return Error::none;
  }

  [[nodiscard]] Error concatenate(const char* destination,
                                  const char* source) {
    if (!valid_path(destination) || !valid_path(source)) {
      return Error::invalid_argument;
    }
    const u32 destination_slot = find(destination);
    const u32 source_slot = find(source);
    if (destination_slot == MaxFiles || source_slot == MaxFiles) {
      return Error::not_found;
    }
    const u32 source_size = files_[source_slot].size;
    if (source_size > MaxFileBytes - files_[destination_slot].size) {
      return Error::no_space;
    }
    const u32 old_size = files_[destination_slot].size;
    if (destination_slot == source_slot) {
      for (u32 index = 0; index < source_size; ++index) {
        files_[destination_slot].data[old_size + index] =
            files_[destination_slot].data[index];
      }
    } else {
      copy(files_[destination_slot].data + old_size,
           files_[source_slot].data, source_size);
    }
    files_[destination_slot].size += source_size;
    return Error::none;
  }

  [[nodiscard]] Error remove(const char* path) {
    if (!valid_path(path)) {
      return Error::invalid_argument;
    }
    const u32 slot = find(path);
    if (slot == MaxFiles) {
      return Error::not_found;
    }
    for (u32 index = 0; index < files_[slot].size; ++index) {
      files_[slot].data[index] = 0;
    }
    files_[slot].path[0] = '\0';
    files_[slot].size = 0;
    files_[slot].used = false;
    return Error::none;
  }

  [[nodiscard]] Result<u32> read(const char* path, u64 offset, u8* output,
                                 u32 size) const {
    if (!valid_path(path) || (output == nullptr && size != 0)) {
      return Result<u32>::failure(Error::invalid_argument);
    }
    const u32 slot = find(path);
    if (slot == MaxFiles) {
      return Result<u32>::failure(Error::not_found);
    }
    if (offset >= files_[slot].size || size == 0) {
      return Result<u32>::success(0);
    }
    u32 count = files_[slot].size - static_cast<u32>(offset);
    if (count > size) {
      count = size;
    }
    copy(output, files_[slot].data + static_cast<u32>(offset), count);
    return Result<u32>::success(count);
  }

  [[nodiscard]] Result<u32> file_size(const char* path) const {
    if (!valid_path(path)) {
      return Result<u32>::failure(Error::invalid_argument);
    }
    const u32 slot = find(path);
    return slot == MaxFiles
               ? Result<u32>::failure(Error::not_found)
               : Result<u32>::success(files_[slot].size);
  }

  [[nodiscard]] bool consistent() const {
    for (u32 left = 0; left < MaxFiles; ++left) {
      if (!files_[left].used) {
        if (files_[left].size != 0 || files_[left].path[0] != '\0') {
          return false;
        }
        continue;
      }
      if (!valid_path(files_[left].path) ||
          files_[left].size > MaxFileBytes) {
        return false;
      }
      for (u32 right = left + 1; right < MaxFiles; ++right) {
        if (files_[right].used &&
            paths_equal(files_[left].path, files_[right].path)) {
          return false;
        }
      }
    }
    return true;
  }

 private:
  [[nodiscard]] static constexpr char folded(char value) {
    if constexpr (FoldAsciiCase) {
      return value >= 'a' && value <= 'z'
                 ? static_cast<char>(value - ('a' - 'A'))
                 : value;
    }
    return value;
  }

  [[nodiscard]] static bool paths_equal(const char* left,
                                        const char* right) {
    u32 index = 0;
    while (left[index] != '\0' && right[index] != '\0') {
      if (folded(left[index]) != folded(right[index])) {
        return false;
      }
      ++index;
    }
    return left[index] == right[index];
  }

  [[nodiscard]] static bool valid_path(const char* path) {
    if (path == nullptr || path[0] != '/' || path[1] == '\0') {
      return false;
    }
    u32 size = 0;
    while (path[size] != '\0') {
      if (++size > MaxPathBytes) {
        return false;
      }
    }
    return true;
  }

  static void copy(u8* destination, const u8* source, u32 size) {
    for (u32 index = 0; index < size; ++index) {
      destination[index] = source[index];
    }
  }

  static void copy_path(char* destination, const char* source) {
    u32 index = 0;
    do {
      destination[index] = source[index];
    } while (source[index++] != '\0');
  }

  [[nodiscard]] u32 unused() const {
    for (u32 slot = 0; slot < MaxFiles; ++slot) {
      if (!files_[slot].used) {
        return slot;
      }
    }
    return MaxFiles;
  }

  [[nodiscard]] u32 find(const char* path) const {
    for (u32 slot = 0; slot < MaxFiles; ++slot) {
      if (files_[slot].used && paths_equal(files_[slot].path, path)) {
        return slot;
      }
    }
    return MaxFiles;
  }

  File files_[MaxFiles]{};
};

}  // namespace mikos::drivers::fs
