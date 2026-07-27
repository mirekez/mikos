#include <mikos/base.hpp>

extern "C" void* memcpy(void* destination, const void* source,
                        mikos::usize size) {
  auto* out = static_cast<mikos::u8*>(destination);
  const auto* in = static_cast<const mikos::u8*>(source);
  for (mikos::usize i = 0; i < size; ++i) {
    out[i] = in[i];
  }
  return destination;
}

extern "C" void* memmove(void* destination, const void* source,
                         mikos::usize size) {
  auto* out = static_cast<mikos::u8*>(destination);
  const auto* in = static_cast<const mikos::u8*>(source);
  if (out < in) {
    for (mikos::usize i = 0; i < size; ++i) {
      out[i] = in[i];
    }
  } else if (out > in) {
    for (mikos::usize i = size; i != 0; --i) {
      out[i - 1] = in[i - 1];
    }
  }
  return destination;
}

extern "C" void* memset(void* destination, int value, mikos::usize size) {
  auto* out = static_cast<mikos::u8*>(destination);
  for (mikos::usize i = 0; i < size; ++i) {
    out[i] = static_cast<mikos::u8>(value);
  }
  return destination;
}

extern "C" int memcmp(const void* left, const void* right, mikos::usize size) {
  const auto* a = static_cast<const mikos::u8*>(left);
  const auto* b = static_cast<const mikos::u8*>(right);
  for (mikos::usize i = 0; i < size; ++i) {
    if (a[i] != b[i]) {
      return static_cast<int>(a[i]) - static_cast<int>(b[i]);
    }
  }
  return 0;
}
