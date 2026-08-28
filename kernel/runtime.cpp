#include <mikos/base.hpp>

extern "C" void* memcpy(void* destination, const void* source,
                        mikos::usize size) {
  auto* out = static_cast<mikos::u8*>(destination);
  const auto* in = static_cast<const mikos::u8*>(source);
  using Word [[gnu::may_alias]] = mikos::u32;
  if (((reinterpret_cast<mikos::usize>(out) |
        reinterpret_cast<mikos::usize>(in)) &
       (sizeof(Word) - 1)) == 0) {
    constexpr mikos::usize words_per_block = 8;
    constexpr mikos::usize block_size = words_per_block * sizeof(Word);
    while (size >= block_size) {
      auto* word_out = reinterpret_cast<Word*>(out);
      const auto* word_in = reinterpret_cast<const Word*>(in);
      word_out[0] = word_in[0];
      word_out[1] = word_in[1];
      word_out[2] = word_in[2];
      word_out[3] = word_in[3];
      word_out[4] = word_in[4];
      word_out[5] = word_in[5];
      word_out[6] = word_in[6];
      word_out[7] = word_in[7];
      out += block_size;
      in += block_size;
      size -= block_size;
    }
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

namespace {

[[nodiscard]] mikos::u64 divide_u64(mikos::u64 dividend,
                                    mikos::u64 divisor,
                                    mikos::u64* remainder) {
  if (divisor == 0) {
    for (;;) {
      asm volatile("wfi");
    }
  }
  mikos::u64 quotient = 0;
  mikos::u64 current = 0;
  for (mikos::u32 bit = 0; bit < 64; ++bit) {
    current = (current << 1) | (dividend >> 63);
    dividend <<= 1;
    quotient <<= 1;
    if (current >= divisor) {
      current -= divisor;
      quotient |= 1;
    }
  }
  if (remainder != nullptr) {
    *remainder = current;
  }
  return quotient;
}

}  // namespace

extern "C" mikos::u64 __udivdi3(mikos::u64 dividend,
                                  mikos::u64 divisor) {
  return divide_u64(dividend, divisor, nullptr);
}

extern "C" mikos::u64 __umoddi3(mikos::u64 dividend,
                                  mikos::u64 divisor) {
  mikos::u64 remainder{};
  static_cast<void>(divide_u64(dividend, divisor, &remainder));
  return remainder;
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
