#pragma once

#include <mikos/base.hpp>

namespace mikos::drivers::tribe_sd {

inline constexpr u32 block_size = 512;

[[nodiscard]] bool initialize();
[[nodiscard]] u64 sector_count();
[[nodiscard]] bool read_block(u32 block, u8* data);
[[nodiscard]] bool read_blocks(u32 block, u8* data, u32 byte_count);
[[nodiscard]] bool write_block(u32 block, const u8* data);
[[nodiscard]] bool write_blocks(u32 block, const u8* data, u32 byte_count);
[[nodiscard]] bool flush();

}  // namespace mikos::drivers::tribe_sd
