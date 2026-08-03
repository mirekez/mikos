#pragma once

#include <mikos/base.hpp>

namespace mikos::path {

inline constexpr u32 capacity = 256;

[[nodiscard]] inline bool canonicalize(const char* current_directory,
                                       const char* path, char* output,
                                       u32 output_capacity) {
  if (current_directory == nullptr || path == nullptr || output == nullptr ||
      path[0] == '\0' || output_capacity < 2) {
    return false;
  }
  char combined[capacity * 2]{};
  u32 combined_size = 0;
  if (path[0] != '/') {
    while (current_directory[combined_size] != '\0') {
      if (combined_size + 1 >= sizeof(combined)) {
        return false;
      }
      combined[combined_size] = current_directory[combined_size];
      ++combined_size;
    }
    if (combined_size == 0 || combined[combined_size - 1] != '/') {
      combined[combined_size++] = '/';
    }
  }
  for (u32 index = 0; path[index] != '\0'; ++index) {
    if (combined_size + 1 >= sizeof(combined)) {
      return false;
    }
    combined[combined_size++] = path[index];
  }
  combined[combined_size] = '\0';

  u32 output_size = 1;
  output[0] = '/';
  u32 cursor = 0;
  while (combined[cursor] != '\0') {
    while (combined[cursor] == '/') {
      ++cursor;
    }
    const u32 component = cursor;
    while (combined[cursor] != '\0' && combined[cursor] != '/') {
      ++cursor;
    }
    const u32 size = cursor - component;
    if (size == 0 || (size == 1 && combined[component] == '.')) {
      continue;
    }
    if (size == 2 && combined[component] == '.' &&
        combined[component + 1] == '.') {
      if (output_size > 1) {
        while (output_size > 1 && output[output_size - 1] != '/') {
          --output_size;
        }
        if (output_size > 1) {
          --output_size;
        }
      }
      continue;
    }
    const u32 separator = output_size > 1 ? 1 : 0;
    if (size + separator >= output_capacity - output_size) {
      return false;
    }
    if (separator != 0) {
      output[output_size++] = '/';
    }
    for (u32 index = 0; index < size; ++index) {
      output[output_size++] = combined[component + index];
    }
  }
  output[output_size] = '\0';
  return true;
}

}  // namespace mikos::path
