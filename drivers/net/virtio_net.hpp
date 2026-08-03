#pragma once

#include <drivers/net/net.hpp>
#include <drivers/net/virtio.hpp>

extern "C" void* memcpy(void*, const void*, mikos::usize);
extern "C" void* memset(void*, int, mikos::usize);

namespace mikos::drivers::virtio {

template <typename Transport, typename Queue>
concept NetTransport = requires(Transport transport, const Queue& queue,
                                MacAddress& mac, u16 number) {
  transport.begin();
  transport.configure_queue(number, queue);
  transport.finish(mac);
  transport.fail();
  transport.notify(number);
};

template <typename Transport, typename Queue, u32 HeaderSize,
          u16 ReceiveBuffers>
  requires NetTransport<Transport, Queue>
class BasicNetDevice {
  static constexpr u32 frame_capacity = 1514;
  using Packet = PacketBuffer<HeaderSize, frame_capacity>;

 public:
  [[nodiscard]] bool initialize() {
    if (!transport_.begin()) {
      return false;
    }
    receive_queue_.reset();
    transmit_queue_.reset();
    if (!transport_.configure_queue(0, receive_queue_) ||
        !transport_.configure_queue(1, transmit_queue_)) {
      transport_.fail();
      return false;
    }
    for (u16 id = 0; id < ReceiveBuffers; ++id) {
      receive_queue_.describe(id, &receive_buffers_[id], sizeof(Packet),
                              Access::device_writes);
      receive_queue_.offer(id, Transport::dma_fence);
    }
    if (!transport_.finish(mac_)) {
      transport_.fail();
      return false;
    }
    Transport::dma_fence();
    transport_.notify(0);
    return true;
  }

  [[nodiscard]] MacAddress mac_address() const { return mac_; }

  [[nodiscard]] bool receive(net::Frame& frame) {
    const auto completion = receive_queue_.take(Transport::dma_fence);
    if (!completion.ready) {
      return false;
    }
    const auto [id, length] = completion.element;
    if (id >= ReceiveBuffers || length < HeaderSize) {
      return false;
    }
    const u32 payload_size = length - HeaderSize;
    const u32 size = payload_size > frame_capacity ? frame_capacity
                                                    : payload_size;
    memcpy(received_frame_, receive_buffers_[id].frame, size);
    receive_queue_.offer(static_cast<u16>(id), Transport::dma_fence);
    transport_.notify(0);
    frame = net::Frame{received_frame_, size};
    return true;
  }

  [[nodiscard]] bool transmit(const u8* frame, u32 size) {
    if (size > frame_capacity) {
      return false;
    }
    memset(transmit_buffer_.header, 0, sizeof(transmit_buffer_.header));
    memcpy(transmit_buffer_.frame, frame, size);
    transmit_queue_.describe(0, &transmit_buffer_, HeaderSize + size,
                             Access::device_reads);
    transmit_queue_.offer(0, Transport::dma_fence);
    transport_.notify(1);
    for (u32 spin = 0; spin < 1'000'000; ++spin) {
      if (transmit_queue_.take(Transport::dma_fence).ready) {
        return true;
      }
    }
    return false;
  }

 private:
  Transport transport_{};
  Queue receive_queue_{};
  Queue transmit_queue_{};
  Packet receive_buffers_[ReceiveBuffers]{};
  Packet transmit_buffer_{};
  u8 received_frame_[frame_capacity]{};
  MacAddress mac_{};
};

}  // namespace mikos::drivers::virtio
