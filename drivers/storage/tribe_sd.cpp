#include <mikos/drivers/tribe_sd.hpp>

namespace mikos::drivers::tribe_sd {
namespace {

inline constexpr u32 base = 0x8200d100;
inline constexpr u32 control = 0x00;
inline constexpr u32 status = 0x04;
inline constexpr u32 command = 0x08;
inline constexpr u32 argument = 0x0c;
inline constexpr u32 length = 0x10;
inline constexpr u32 receive_data = 0x1c;

inline constexpr u32 control_start = 1u << 0;
inline constexpr u32 control_clear_done = 1u << 4;
inline constexpr u32 status_done = 1u << 1;
inline constexpr u32 status_error = 1u << 2;
inline constexpr u32 status_rx_valid = 1u << 3;
inline constexpr u32 read_single_block = 0x51;
inline constexpr u32 timeout = 2'000'000;

[[nodiscard]] volatile u32& reg(u32 offset) {
  return *reinterpret_cast<volatile u32*>(base + offset);
}

void fence() { asm volatile("fence iorw, iorw" ::: "memory"); }

[[nodiscard]] bool wait_for(u32 mask) {
  for (u32 spin = 0; spin < timeout; ++spin) {
    if ((reg(status) & mask) != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool initialize() {
  reg(control) = control_clear_done;
  fence();
  return (reg(status) & status_error) == 0;
}

bool read_block(u32 block, u8* data) {
  if (data == nullptr) {
    return false;
  }
  reg(control) = control_clear_done;
  reg(command) = read_single_block;
  reg(argument) = block;
  reg(length) = block_size;
  reg(control) = control_start;
  fence();

  for (u32 i = 0; i < block_size; ++i) {
    if (!wait_for(status_rx_valid)) {
      return false;
    }
    data[i] = static_cast<u8>(reg(receive_data));
  }
  if (!wait_for(status_done)) {
    return false;
  }
  fence();
  return (reg(status) & status_error) == 0;
}

}  // namespace mikos::drivers::tribe_sd
