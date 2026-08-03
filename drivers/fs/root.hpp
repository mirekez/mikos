#pragma once

#include <drivers/fs/ext4/ext4.hpp>

namespace mikos::drivers::fs::root {

using Node = ext4::Node;
using Entry = ext4::Entry;
using Type = ext4::Type;
using DirectoryVisitor = bool (*)(void* context, const Entry& entry);

[[nodiscard]] bool initialize();
[[nodiscard]] Result<Node> lookup(const char* path);
[[nodiscard]] Error for_each(const Node& directory, void* context,
                             DirectoryVisitor visitor);
[[nodiscard]] Result<u32> read(const Node& file, u64 offset, u8* output,
                               u32 count);
[[nodiscard]] Result<u32> write(Node& file, u64 offset, const u8* input,
                                u32 count);
[[nodiscard]] Error create(const char* path, const u8* input, u32 count);
[[nodiscard]] Error mkdir(const char* path, u16 mode);
[[nodiscard]] Error truncate(Node& file, u64 size);
[[nodiscard]] Error remove(const char* path);
[[nodiscard]] Error move(const char* source, const char* destination);
[[nodiscard]] Error sync();

}  // namespace mikos::drivers::fs::root
