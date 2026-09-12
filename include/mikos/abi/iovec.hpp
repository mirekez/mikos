#pragma once

#include <mikos/base.hpp>
#include <type_traits>

namespace mikos::abi {
// Linux RV32 wire record. Packed permits byte-aligned user buffers; the kernel
// snapshots records before interpreting them. No ownership or virtual methods.
struct [[gnu::packed]] Iovec32 {
  u32 base;
  u32 size;
};
static_assert(sizeof(Iovec32) == 8);
static_assert(std::is_trivially_copyable_v<Iovec32> &&
              std::is_standard_layout_v<Iovec32>);
}  // namespace mikos::abi
