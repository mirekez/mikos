#include <drivers/storage/tribe_sd.hpp>
#include <drivers/storage/tribe_sd_dma.hpp>

namespace mikos::drivers::tribe_sd {
namespace {

inline constexpr u32 base = 0x8200d100;
inline constexpr u32 receive_data = 0x1c;
inline constexpr u32 status_rx_valid = 1u << 3;
inline constexpr u32 pio_timeout = 2'000'000;
[[nodiscard]] volatile u32& reg(u32 offset) {
  return *reinterpret_cast<volatile u32*>(base + offset);
}

struct Registers {
  void write(u32 offset, u32 value) {
    reg(offset) = value;
  }

  [[nodiscard]] u32 read(u32 offset) const {
    return reg(offset);
  }

  void fence() const { asm volatile("fence iorw, iorw" ::: "memory"); }
};

[[nodiscard]] bool wait_for(Registers& registers, u32 mask) {
  for (u32 spin = 0; spin < pio_timeout; ++spin) {
    if ((registers.read(detail::status) & mask) != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool initialize() {
  Registers registers;
  registers.write(detail::control, detail::control_clear_done);
  registers.fence();
  return (registers.read(detail::status) & detail::status_error) == 0;
}

u64 sector_count() {
  // The controller uses a 32-bit logical block address and reports media
  // errors for sectors beyond the attached image.
  return u64{1} << 32;
}

bool read_block(u32 block, u8* data) {
  if (data == nullptr) {
    return false;
  }
  Registers registers;
  registers.write(detail::control, detail::control_clear_done);
  registers.write(detail::command, detail::read_single_block);
  registers.write(detail::argument, block);
  registers.write(detail::length, block_size);
  registers.write(detail::control, detail::control_start);
  registers.fence();

  for (u32 index = 0; index < block_size; ++index) {
    if (!wait_for(registers, status_rx_valid)) {
      return false;
    }
    data[index] = static_cast<u8>(registers.read(receive_data));
  }
  if (!wait_for(registers, detail::status_done)) {
    return false;
  }
  registers.fence();
  return (registers.read(detail::status) & detail::status_error) == 0;
}

bool read_blocks(u32 block, u8* data, u32 byte_count) {
  const usize address = reinterpret_cast<usize>(data);
  if (data == nullptr || address > 0xffffffffu || byte_count == 0 ||
      byte_count % block_size != 0 ||
      byte_count / block_size > 0xffffffffu - block) {
    return false;
  }
  Registers registers;
  return detail::read_dma(registers, block, static_cast<u32>(address),
                          byte_count);
}

bool write_block(u32 block, const u8* data) {
  return write_blocks(block, data, block_size);
}

bool write_blocks(u32 block, const u8* data, u32 byte_count) {
  const usize address = reinterpret_cast<usize>(data);
  if (data == nullptr || address > 0xffffffffu || byte_count == 0 ||
      byte_count % block_size != 0 ||
      byte_count / block_size > 0xffffffffu - block) {
    return false;
  }
  Registers registers;
  return detail::write_dma(registers, block, static_cast<u32>(address),
                           byte_count);
}

bool flush() {
  // Controller completion is synchronous with the card-model update.
  asm volatile("fence iorw, iorw" ::: "memory");
  return true;
}

}  // namespace mikos::drivers::tribe_sd
