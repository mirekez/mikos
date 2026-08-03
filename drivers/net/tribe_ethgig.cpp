#include <drivers/net/net.hpp>

namespace mikos::drivers::net {
namespace {

inline constexpr u32 mac_base = 0x8200e000;
inline constexpr u32 dma_base = 0x8200e100;

inline constexpr u32 tx_control = 0x00;
inline constexpr u32 tx_status = 0x04;
inline constexpr u32 tx_current = 0x08;
inline constexpr u32 tx_tail = 0x10;
inline constexpr u32 rx_control = 0x30;
inline constexpr u32 rx_status = 0x34;
inline constexpr u32 rx_current = 0x38;
inline constexpr u32 rx_tail = 0x40;

inline constexpr u32 dma_run = 1u << 0;
inline constexpr u32 dma_reset = 1u << 2;
inline constexpr u32 dma_irq_complete = 1u << 12;
inline constexpr u32 dma_irq_error = 1u << 14;
inline constexpr u32 dma_irq_all = 7u << 12;

inline constexpr u32 descriptor_next = 0x00;
inline constexpr u32 descriptor_buffer = 0x08;
inline constexpr u32 descriptor_control = 0x18;
inline constexpr u32 descriptor_status = 0x1c;
inline constexpr u32 descriptor_tx_start = 1u << 27;
inline constexpr u32 descriptor_tx_end = 1u << 26;
inline constexpr u32 descriptor_done = 1u << 31;
inline constexpr u32 descriptor_length = 0x007fffff;

inline constexpr u32 mac_unicast_low = 0x700;
inline constexpr u32 mac_unicast_high = 0x704;
inline constexpr u32 buffer_size = 1536;
inline constexpr u32 timeout = 5'000'000;
inline constexpr MacAddress local_mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};

struct alignas(64) Descriptor {
  u32 word[16];
};

Descriptor rx_descriptor{};
Descriptor tx_descriptor{};
alignas(64) u8 rx_buffer[buffer_size]{};
alignas(64) u8 tx_buffer[buffer_size]{};
bool rx_posted{};

[[nodiscard]] volatile u32& dma_reg(u32 offset) {
  return *reinterpret_cast<volatile u32*>(dma_base + offset);
}

[[nodiscard]] volatile u32& mac_reg(u32 offset) {
  return *reinterpret_cast<volatile u32*>(mac_base + offset);
}

void fence() { asm volatile("fence iorw, iorw" ::: "memory"); }

void clear(Descriptor& descriptor) {
  for (auto& value : descriptor.word) {
    value = 0;
  }
}

void post_receive() {
  clear(rx_descriptor);
  rx_descriptor.word[descriptor_next / 4] = 0;
  rx_descriptor.word[descriptor_buffer / 4] =
      static_cast<u32>(reinterpret_cast<usize>(rx_buffer));
  rx_descriptor.word[descriptor_control / 4] = buffer_size;
  fence();
  dma_reg(rx_status) = dma_irq_all;
  dma_reg(rx_current) =
      static_cast<u32>(reinterpret_cast<usize>(&rx_descriptor));
  dma_reg(rx_control) = dma_run | dma_irq_complete | dma_irq_error;
  dma_reg(rx_tail) =
      static_cast<u32>(reinterpret_cast<usize>(&rx_descriptor));
  fence();
  rx_posted = true;
}

[[nodiscard]] bool wait_for_tx() {
  for (u32 spin = 0; spin < timeout; ++spin) {
    fence();
    const u32 status = dma_reg(tx_status);
    if ((status & dma_irq_error) != 0) {
      dma_reg(tx_status) = dma_irq_all;
      return false;
    }
    if ((status & dma_irq_complete) != 0) {
      dma_reg(tx_status) = dma_irq_all;
      return true;
    }
  }
  return false;
}

}  // namespace

bool initialize() {
  dma_reg(tx_control) = dma_reset;
  dma_reg(rx_control) = dma_reset;
  dma_reg(tx_status) = dma_irq_all;
  dma_reg(rx_status) = dma_irq_all;

  // Tribe exposes the Xilinx AXI Ethernet unicast address registers in the
  // same 4 KiB window as its simple DMA engine.
  mac_reg(mac_unicast_low) = 0x00000002;
  mac_reg(mac_unicast_high) = 0x00000200;
  fence();
  post_receive();
  return true;
}

MacAddress mac_address() { return local_mac; }

bool receive(Frame& frame) {
  if (!rx_posted) {
    post_receive();
  }
  fence();
  const u32 status = rx_descriptor.word[descriptor_status / 4];
  if ((status & descriptor_done) == 0) {
    return false;
  }
  rx_posted = false;
  if ((dma_reg(rx_status) & dma_irq_error) != 0) {
    return false;
  }
  const u32 size = status & descriptor_length;
  if (size < 14 || size > buffer_size) {
    return false;
  }
  frame = Frame{rx_buffer, size};
  return true;
}

bool transmit(const u8* frame, u32 size) {
  if (frame == nullptr || size < 14 || size > buffer_size) {
    return false;
  }
  for (u32 i = 0; i < size; ++i) {
    tx_buffer[i] = frame[i];
  }
  clear(tx_descriptor);
  tx_descriptor.word[descriptor_next / 4] = 0;
  tx_descriptor.word[descriptor_buffer / 4] =
      static_cast<u32>(reinterpret_cast<usize>(tx_buffer));
  tx_descriptor.word[descriptor_control / 4] =
      descriptor_tx_start | descriptor_tx_end | size;
  fence();

  dma_reg(tx_status) = dma_irq_all;
  dma_reg(tx_current) =
      static_cast<u32>(reinterpret_cast<usize>(&tx_descriptor));
  dma_reg(tx_control) = dma_run | dma_irq_complete | dma_irq_error;
  dma_reg(tx_tail) =
      static_cast<u32>(reinterpret_cast<usize>(&tx_descriptor));
  fence();
  return wait_for_tx();
}

}  // namespace mikos::drivers::net
