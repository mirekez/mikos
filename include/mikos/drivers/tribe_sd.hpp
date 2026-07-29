#pragma once

#include <mikos/base.hpp>

namespace mikos::drivers::tribe_sd {

inline constexpr u32 block_size = 512;

[[nodiscard]] bool initialize();
[[nodiscard]] bool read_block(u32 block, u8* data);

}  // namespace mikos::drivers::tribe_sd
