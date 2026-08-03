#pragma once

#include <mikos/base.hpp>

namespace mikos::drivers::virtio {

enum class Access : u16 {
  device_reads = 0,
  device_writes = 2,
};

enum class Status : u32 {
  acknowledge = 1,
  driver = 2,
  ready = 4,
  features_ok = 8,
  failed = 128,
};

enum class Feature : u8 {
  mac = 5,
  block_flush = 9,
  version_1 = 32,
};

template <typename... Values>
[[nodiscard]] constexpr u32 bits(Values... values) {
  return (0u | ... | static_cast<u32>(values));
}

class FeatureSet {
 public:
  constexpr FeatureSet() = default;

  template <typename... Rest>
  explicit constexpr FeatureSet(Feature first, Rest... rest)
      : value_{bit(first) | (0ull | ... | bit(rest))} {}

  [[nodiscard]] static constexpr FeatureSet from_banks(u32 low, u32 high) {
    FeatureSet result;
    result.value_ = static_cast<u64>(low) | (static_cast<u64>(high) << 32);
    return result;
  }

  [[nodiscard]] constexpr bool contains(Feature feature) const {
    return (value_ & bit(feature)) != 0;
  }

  [[nodiscard]] constexpr u32 low() const { return static_cast<u32>(value_); }

  [[nodiscard]] constexpr u32 high() const {
    return static_cast<u32>(value_ >> 32);
  }

  [[nodiscard]] friend constexpr FeatureSet operator&(FeatureSet left,
                                                       FeatureSet right) {
    FeatureSet result;
    result.value_ = left.value_ & right.value_;
    return result;
  }

 private:
  [[nodiscard]] static constexpr u64 bit(Feature feature) {
    return u64{1} << static_cast<u8>(feature);
  }

  u64 value_{};
};

struct [[gnu::packed]] Descriptor {
  u64 address;
  u32 length;
  u16 flags;
  u16 next;
};

struct [[gnu::packed]] UsedElement {
  u32 id;
  u32 length;
};

struct UsedResult {
  bool ready;
  UsedElement element;
};

template <u16 Size>
class alignas(16) SplitQueue {
  struct [[gnu::packed]] Available {
    u16 flags;
    u16 index;
    u16 ring[Size];
    u16 used_event;
  };

  struct [[gnu::packed]] Used {
    u16 flags;
    u16 index;
    UsedElement ring[Size];
    u16 available_event;
  };

 public:
  static constexpr u16 size = Size;

  constexpr void reset() {
    for (auto& descriptor : descriptors_) {
      descriptor = {};
    }
    available_ = {};
    used_.flags = 0;
    used_.index = 0;
    used_.available_event = 0;
    for (u16 i = 0; i < Size; ++i) {
      used_.ring[i].id = 0;
      used_.ring[i].length = 0;
    }
    seen_used_ = 0;
  }

  constexpr void describe(u16 id, const void* address, u32 length,
                          Access access, u16 next = Size) {
    const u16 chain = next < Size ? 1u : 0u;
    descriptors_[id] = Descriptor{reinterpret_cast<usize>(address), length,
                                  static_cast<u16>(
                                      static_cast<u16>(access) | chain),
                                  static_cast<u16>(next < Size ? next : 0)};
  }

  template <typename Barrier>
  constexpr void offer(u16 id, Barrier barrier) {
    const u16 index = available_.index;
    available_.ring[index % Size] = id;
    barrier();
    available_.index = static_cast<u16>(index + 1);
    barrier();
  }

  template <typename Barrier>
  [[nodiscard]] constexpr UsedResult take(Barrier barrier) {
    barrier();
    if (seen_used_ == used_.index) {
      return {};
    }
    const auto& entry = used_.ring[seen_used_ % Size];
    const UsedElement element{entry.id, entry.length};
    ++seen_used_;
    return {true, element};
  }

  [[nodiscard]] constexpr usize descriptor_address() const {
    return reinterpret_cast<usize>(descriptors_);
  }

  [[nodiscard]] constexpr usize available_address() const {
    return reinterpret_cast<usize>(&available_);
  }

  [[nodiscard]] constexpr usize used_address() const {
    return reinterpret_cast<usize>(&used_);
  }

  [[nodiscard]] constexpr const Descriptor& descriptor(u16 id) const {
    return descriptors_[id];
  }

 private:
  Descriptor descriptors_[Size]{};
  Available available_{};
  alignas(4) volatile Used used_{};
  u16 seen_used_{};
};

template <u16 Size, u32 Alignment = 4096>
class alignas(Alignment) LegacySplitQueue {
  static constexpr u32 descriptor_bytes = sizeof(Descriptor) * Size;
  static constexpr u32 available_bytes = sizeof(u16) * (3 + Size);
  static constexpr u32 used_offset =
      align_up(descriptor_bytes + available_bytes, Alignment);
  static constexpr u32 used_bytes = sizeof(u16) * 3 +
                                    sizeof(UsedElement) * Size;
  static constexpr u32 storage_size =
      align_up(used_offset + used_bytes, Alignment);

 public:
  static constexpr u16 size = Size;

  constexpr void reset() {
    for (auto& byte : storage_) {
      byte = 0;
    }
    seen_used_ = 0;
  }

  constexpr void describe(u16 id, const void* address, u32 length,
                          Access access) {
    descriptors()[id] = Descriptor{reinterpret_cast<usize>(address), length,
                                   static_cast<u16>(access), 0};
  }

  template <typename Barrier>
  constexpr void offer(u16 id, Barrier barrier) {
    auto* ring = available();
    const u16 index = ring[1];
    ring[2 + index % Size] = id;
    barrier();
    ring[1] = static_cast<u16>(index + 1);
    barrier();
  }

  template <typename Barrier>
  [[nodiscard]] constexpr UsedResult take(Barrier barrier) {
    auto* ring = used();
    barrier();
    if (seen_used_ == ring[1]) {
      return {};
    }
    auto* elements = reinterpret_cast<volatile UsedElement*>(ring + 2);
    const auto& entry = elements[seen_used_ % Size];
    const UsedElement element{entry.id, entry.length};
    ++seen_used_;
    return {true, element};
  }

  [[nodiscard]] constexpr usize physical_address() const {
    return reinterpret_cast<usize>(storage_);
  }

 private:
  [[nodiscard]] constexpr Descriptor* descriptors() {
    return reinterpret_cast<Descriptor*>(storage_);
  }

  [[nodiscard]] constexpr u16* available() {
    return reinterpret_cast<u16*>(storage_ + descriptor_bytes);
  }

  [[nodiscard]] constexpr volatile u16* used() {
    return reinterpret_cast<volatile u16*>(storage_ + used_offset);
  }

  u8 storage_[storage_size]{};
  u16 seen_used_{};
};

template <u32 HeaderSize, u32 FrameSize = 1514>
struct alignas(16) PacketBuffer {
  u8 header[HeaderSize];
  u8 frame[FrameSize];
};

static_assert(sizeof(Descriptor) == 16);

}  // namespace mikos::drivers::virtio
