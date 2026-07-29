#include <mikos/base.hpp>

extern "C" void* memcpy(void* destination, const void* source,
                        mikos::usize size) {
  auto* out = static_cast<mikos::u8*>(destination);
  const auto* in = static_cast<const mikos::u8*>(source);
  using Word [[gnu::may_alias]] = mikos::u32;
  if (((reinterpret_cast<mikos::usize>(out) |
        reinterpret_cast<mikos::usize>(in)) &
       (sizeof(Word) - 1)) == 0) {
    while (size >= sizeof(Word)) {
      *reinterpret_cast<Word*>(out) = *reinterpret_cast<const Word*>(in);
      out += sizeof(Word);
      in += sizeof(Word);
      size -= sizeof(Word);
    }
  }
  while (size != 0) {
    *out++ = *in++;
    --size;
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
  const auto byte = static_cast<mikos::u8>(value);
  while (size != 0 &&
         (reinterpret_cast<mikos::usize>(out) &
          (sizeof(mikos::u32) - 1)) != 0) {
    *out++ = byte;
    --size;
  }
  const mikos::u32 word = static_cast<mikos::u32>(byte) * 0x01010101u;
  while (size >= sizeof(word)) {
    *reinterpret_cast<mikos::u32*>(out) = word;
    out += sizeof(word);
    size -= sizeof(word);
  }
  while (size != 0) {
    *out++ = byte;
    --size;
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
