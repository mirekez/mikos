#include <drivers/uart/uart.hpp>

namespace mikos::drivers::uart {
namespace {

inline constexpr u32 base = 0x82000000;

[[nodiscard]] volatile u8& reg(u32 offset) {
  return *reinterpret_cast<volatile u8*>(base + offset);
}

}  // namespace

bool initialize() { return (reg(5) & 0x60u) != 0; }

void put(u8 value) {
  while ((reg(5) & 0x20u) == 0) {
  }
  reg(0) = value;
}

bool ready() { return (reg(5) & 1u) != 0; }

bool receive(u8& value) {
  if (!ready()) {
    return false;
  }
  value = reg(0);
  return true;
}

}  // namespace mikos::drivers::uart
