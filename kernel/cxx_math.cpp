#include <mikos/base.hpp>
#include <bit>

// IEEE-754 binary32 ceiling for libc++ hash-table load factors. Integer-only;
// preserves signed zero and NaN payloads. No floating-point environment exists
// in the kernel, so floating-point status flags are deliberately not raised.
extern "C" float ceilf(float value) noexcept {
  auto bits = std::bit_cast<mikos::u32>(value);
  const auto magnitude = bits & 0x7fffffffu;
  const auto exponent = magnitude >> 23;
  if (exponent >= 150 || magnitude == 0) return value;
  if (exponent < 127)
    return std::bit_cast<float>((bits >> 31) ? 0x80000000u : 0x3f800000u);
  const mikos::u32 mask = (mikos::u32{1} << (150 - exponent)) - 1;
  if ((bits & mask) == 0) return value;
  if ((bits >> 31) == 0) bits += mask;
  return std::bit_cast<float>(bits & ~mask);
}
