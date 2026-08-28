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
// A normal SSH KEX response can arrive as three distinct frames before the
// slow cycle model next polls us: ACK(server KEX reply), client KEX payload,
// ACK(server NEWKEYS). Two descriptors lose the payload between those ACKs.
// Leave additional headroom for retransmissions and channel/PTY bursts.
inline constexpr u32 receive_descriptor_count = 8;
static_assert(receive_descriptor_count >= 4);
inline constexpr u32 timeout = 5'000'000;
inline constexpr MacAddress local_mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};

struct alignas(64) Descriptor {
  u32 word[16];
};

Descriptor rx_descriptors[receive_descriptor_count]{};
Descriptor tx_descriptor{};
alignas(64) u8 rx_buffers[receive_descriptor_count][buffer_size]{};
alignas(64) u8 tx_buffer[buffer_size]{};
bool rx_posted{};
u32 rx_consume_index{};
u32 rx_rearm_index{};

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

void prepare_receive(u32 index) {
  auto& descriptor = rx_descriptors[index];
  clear(descriptor);
  descriptor.word[descriptor_next / 4] = static_cast<u32>(
      reinterpret_cast<usize>(
          &rx_descriptors[(index + 1) % receive_descriptor_count]));
  descriptor.word[descriptor_buffer / 4] =
      static_cast<u32>(reinterpret_cast<usize>(rx_buffers[index]));
  descriptor.word[descriptor_control / 4] = buffer_size;
}

void initialize_receive_ring() {
  for (u32 index = 0; index < receive_descriptor_count; ++index) {
    prepare_receive(index);
  }
  fence();
  dma_reg(rx_status) = dma_irq_all;
  dma_reg(rx_current) =
      static_cast<u32>(reinterpret_cast<usize>(&rx_descriptors[0]));
  dma_reg(rx_control) = dma_run | dma_irq_complete | dma_irq_error;
  dma_reg(rx_tail) = static_cast<u32>(reinterpret_cast<usize>(
      &rx_descriptors[receive_descriptor_count - 1]));
  fence();
  rx_posted = true;
  rx_consume_index = 0;
}

void rearm_receive() {
  prepare_receive(rx_rearm_index);
  fence();
  dma_reg(rx_status) = dma_irq_all;
  // Moving TDESC to the descriptor just returned to DMA extends the posted
  // ring without disturbing CDESC or a packet already filling its peer.
  dma_reg(rx_tail) = static_cast<u32>(
      reinterpret_cast<usize>(&rx_descriptors[rx_rearm_index]));
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
  initialize_receive_ring();
  return true;
}

MacAddress mac_address() { return local_mac; }

bool receive(Frame& frame) {
  if (!rx_posted) {
    rearm_receive();
    return false;
  }
  fence();
  const u32 dma_status = dma_reg(rx_status);
  if ((dma_status & dma_irq_error) != 0) {
    rx_posted = false;
    rx_rearm_index = rx_consume_index;
    dma_reg(rx_status) = dma_irq_all;
    return false;
  }
  auto& descriptor = rx_descriptors[rx_consume_index];
  const u32 status = descriptor.word[descriptor_status / 4];
  if ((status & descriptor_done) == 0) {
    return false;
  }
  rx_posted = false;
  rx_rearm_index = rx_consume_index;
  rx_consume_index = (rx_consume_index + 1) % receive_descriptor_count;
  const u32 size = status & descriptor_length;
  if (size < 14 || size > buffer_size) {
    return false;
  }
  frame = Frame{rx_buffers[rx_rearm_index], size};
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
