#include <drivers/net/virtio_net.hpp>

namespace mikos::drivers::net {
namespace {

using virtio::Feature;
using virtio::Status;

inline constexpr u16 pci_address_port = 0x0cf8;
inline constexpr u16 pci_data_port = 0x0cfc;
inline constexpr u16 virtio_vendor = 0x1af4;
inline constexpr u16 virtio_legacy_net = 0x1000;

enum class Register : u16 {
  device_features = 0x00,
  driver_features = 0x04,
  queue_pfn = 0x08,
  queue_size = 0x0c,
  queue_select = 0x0e,
  queue_notify = 0x10,
  status = 0x12,
  config = 0x14,
};

void out8(u16 port, u8 value) {
  asm volatile("outb %0, %w1" : : "a"(value), "Nd"(port));
}

void out16(u16 port, u16 value) {
  asm volatile("outw %0, %w1" : : "a"(value), "Nd"(port));
}

void out32(u16 port, u32 value) {
  asm volatile("outl %0, %w1" : : "a"(value), "Nd"(port));
}

[[nodiscard]] u8 in8(u16 port) {
  u8 value;
  asm volatile("inb %w1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

[[nodiscard]] u16 in16(u16 port) {
  u16 value;
  asm volatile("inw %w1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

[[nodiscard]] u32 in32(u16 port) {
  u32 value;
  asm volatile("inl %w1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

class LegacyPci {
 public:
  constexpr LegacyPci() = default;

  [[nodiscard]] constexpr explicit operator bool() const { return base_ != 0; }

  [[nodiscard]] u8 read8(Register reg, u16 extra = 0) const {
    return in8(static_cast<u16>(base_ + static_cast<u16>(reg) + extra));
  }

  [[nodiscard]] u16 read16(Register reg) const {
    return in16(static_cast<u16>(base_ + static_cast<u16>(reg)));
  }

  [[nodiscard]] u32 read32(Register reg) const {
    return in32(static_cast<u16>(base_ + static_cast<u16>(reg)));
  }

  void write8(Register reg, u8 value) const {
    out8(static_cast<u16>(base_ + static_cast<u16>(reg)), value);
  }

  void write16(Register reg, u16 value) const {
    out16(static_cast<u16>(base_ + static_cast<u16>(reg)), value);
  }

  void write32(Register reg, u32 value) const {
    out32(static_cast<u16>(base_ + static_cast<u16>(reg)), value);
  }

  [[nodiscard]] static LegacyPci find_network_device() {
    for (u8 device = 0; device < 32; ++device) {
      for (u8 function = 0; function < 8; ++function) {
        const u32 id = config_read(device, function, 0);
        if (static_cast<u16>(id) != virtio_vendor ||
            static_cast<u16>(id >> 16) != virtio_legacy_net) {
          continue;
        }
        const u32 bar = config_read(device, function, 0x10);
        if ((bar & 1u) == 0) {
          return {};
        }
        enable_io_and_dma(device, function);
        return LegacyPci{static_cast<u16>(bar & ~3u)};
      }
    }
    return {};
  }

 private:
  explicit constexpr LegacyPci(u16 base) : base_{base} {}

  [[nodiscard]] static u32 config_address(u8 device, u8 function, u8 offset) {
    return 0x80000000u | (static_cast<u32>(device) << 11) |
           (static_cast<u32>(function) << 8) | (offset & 0xfcu);
  }

  [[nodiscard]] static u32 config_read(u8 device, u8 function, u8 offset) {
    out32(pci_address_port, config_address(device, function, offset));
    return in32(pci_data_port);
  }

  static void enable_io_and_dma(u8 device, u8 function) {
    const u16 command = static_cast<u16>(config_read(device, function, 0x04));
    out32(pci_address_port, config_address(device, function, 0x04));
    out16(pci_data_port, static_cast<u16>(command | 0x5u));
  }

  u16 base_{};
};

class LegacyTransport {
 public:
  using Queue = virtio::LegacySplitQueue<256>;

  [[nodiscard]] bool begin() {
    pci_ = LegacyPci::find_network_device();
    if (!pci_) {
      return false;
    }
    pci_.write8(Register::status, 0);
    set_status(Status::acknowledge);
    set_status(Status::driver);
    const auto offered = virtio::FeatureSet::from_banks(
        pci_.read32(Register::device_features), 0);
    pci_.write32(Register::driver_features,
                 (offered & virtio::FeatureSet{Feature::mac}).low());
    return true;
  }

  [[nodiscard]] bool configure_queue(u16 number, const Queue& queue) {
    pci_.write16(Register::queue_select, number);
    if (pci_.read16(Register::queue_size) != Queue::size) {
      return false;
    }
    pci_.write32(Register::queue_pfn,
                 static_cast<u32>(queue.physical_address() >> 12));
    return true;
  }

  [[nodiscard]] bool finish(MacAddress& mac) {
    for (u16 i = 0; i < 6; ++i) {
      mac.octet[i] = pci_.read8(Register::config, i);
    }
    set_status(Status::ready);
    return true;
  }

  void fail() { set_status(Status::failed); }
  void notify(u16 queue) { pci_.write16(Register::queue_notify, queue); }

  static void dma_fence() { asm volatile("mfence" ::: "memory"); }

 private:
  void set_status(Status status) {
    pci_.write8(Register::status,
                static_cast<u8>(pci_.read8(Register::status) |
                                virtio::bits(status)));
  }

  LegacyPci pci_{};
};

using Device =
    virtio::BasicNetDevice<LegacyTransport, LegacyTransport::Queue, 10, 8>;
Device device;

}  // namespace

bool initialize() { return device.initialize(); }
MacAddress mac_address() { return device.mac_address(); }
bool receive(Frame& frame) { return device.receive(frame); }
bool transmit(const u8* frame, u32 size) { return device.transmit(frame, size); }

}  // namespace mikos::drivers::net
