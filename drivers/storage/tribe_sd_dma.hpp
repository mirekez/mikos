#pragma once

#include <mikos/base.hpp>

namespace mikos::drivers::tribe_sd::detail {

inline constexpr u32 control = 0x00;
inline constexpr u32 status = 0x04;
inline constexpr u32 command = 0x08;
inline constexpr u32 argument = 0x0c;
inline constexpr u32 length = 0x10;
inline constexpr u32 dma_address = 0x14;

inline constexpr u32 control_start = 1u << 0;
inline constexpr u32 control_write = 1u << 1;
inline constexpr u32 control_dma = 1u << 2;
inline constexpr u32 control_clear_done = 1u << 4;
inline constexpr u32 status_done = 1u << 1;
inline constexpr u32 status_error = 1u << 2;
inline constexpr u32 read_single_block = 0x51;
inline constexpr u32 write_single_block = 0x58;
inline constexpr u32 dma_alignment = 8;
inline constexpr u32 default_timeout = 2'000'000;

template <typename Registers>
[[nodiscard]] bool read_dma(Registers& registers, u32 block,
                            u32 destination, u32 size,
                            u32 timeout = default_timeout) {
  if (destination == 0 || (destination & (dma_alignment - 1)) != 0 ||
      size == 0 || timeout == 0) {
    return false;
  }

  registers.write(control, control_clear_done);
  registers.write(command, read_single_block);
  registers.write(argument, block);
  registers.write(length, size);
  registers.write(dma_address, destination);
  registers.fence();
  registers.write(control, control_start | control_dma);
  registers.fence();

  for (u32 spin = 0; spin < timeout; ++spin) {
    const u32 value = registers.read(status);
    if ((value & status_error) != 0) {
      return false;
    }
    if ((value & status_done) != 0) {
      registers.fence();
      return true;
    }
  }
  return false;
}

template <typename Registers>
[[nodiscard]] bool write_dma(Registers& registers, u32 block, u32 source,
                             u32 size, u32 timeout = default_timeout) {
  if (source == 0 || (source & (dma_alignment - 1)) != 0 || size == 0 ||
      timeout == 0) {
    return false;
  }
  registers.write(control, control_clear_done);
  registers.write(command, write_single_block);
  registers.write(argument, block);
  registers.write(length, size);
  registers.write(dma_address, source);
  registers.fence();
  registers.write(control, control_start | control_write | control_dma);
  registers.fence();
  for (u32 spin = 0; spin < timeout; ++spin) {
    const u32 value = registers.read(status);
    if ((value & status_error) != 0) {
      return false;
    }
    if ((value & status_done) != 0) {
      registers.fence();
      return true;
    }
  }
  return false;
}

}  // namespace mikos::drivers::tribe_sd::detail
