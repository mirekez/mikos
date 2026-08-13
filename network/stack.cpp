#include <drivers/net/net.hpp>
#include <mikos/kernel.hpp>
#include <mikos/net/interface.hpp>
#include <mikos/net/tcp.hpp>

namespace mikos::network {
namespace {

enum class Reply : u8 {
  none,
  arp,
  echo,
};

class BootstrapStack {
 public:
  [[nodiscard]] bool initialize() {
    if (initialized_) {
      return true;
    }
    if (!drivers::net::initialize()) {
      write_text("MIKOS:NET_NONE\n");
      return false;
    }
    state_.mac = drivers::net::mac_address();
    initialized_ = true;
    announce_address();
    return true;
  }

  [[nodiscard]] bool run() {
    if (!initialize()) {
      return false;
    }
    for (u32 spin = 0; spin < 100'000'000; ++spin) {
      poll();
      if (arp_replied_ && echo_replied_) {
        return true;
      }
    }
    write_text("MIKOS:NET_TIMEOUT\n");
    return false;
  }

  void poll() {
    if (!initialized_ || (state_.flags & interface_up) == 0) {
      return;
    }
    flush_acknowledgements();
    drivers::net::Frame frame{};
    if (drivers::net::receive(frame)) {
      record(handle(frame));
    }
    flush_retransmission();
  }

  [[nodiscard]] InterfaceControlResult ioctl(u32 request, Ifreq32& value) {
    if (!initialized_) {
      return InterfaceControlResult::no_device;
    }
    return apply_interface_ioctl(state_, request, value);
  }

  [[nodiscard]] OpenResult socket_open(abi::socket::Type type) {
    return sockets_.open(type);
  }

  [[nodiscard]] SocketResult socket_retain(u8 handle) {
    return sockets_.retain(handle);
  }

  [[nodiscard]] SocketResult socket_close(u8 handle) {
    auto* value = sockets_.slot(handle);
    if (value == nullptr) {
      return SocketResult::bad_handle;
    }
    if (value->references == 1 &&
        (value->state == SocketState::established ||
         value->state == SocketState::close_wait) &&
        !value->send_closed) {
      static_cast<void>(send_segment(*value, tcp_fin | tcp_ack, nullptr, 0,
                                     value->send_next));
      ++value->send_next;
      value->send_closed = true;
    }
    const bool final_reference = value->references == 1;
    const auto result = sockets_.release(handle);
    if (final_reference && result == SocketResult::success) {
      clear_transmissions(handle);
    }
    return result;
  }

  [[nodiscard]] SocketResult socket_bind(u8 handle, Endpoint local) {
    if (!unspecified(local.address) && local.address != state_.address) {
      return SocketResult::invalid_argument;
    }
    return sockets_.bind(handle, local);
  }

  [[nodiscard]] SocketResult socket_listen(u8 handle, u32 backlog) {
    return sockets_.listen(handle, backlog);
  }

  [[nodiscard]] AcceptResult socket_accept(u8 handle) {
    return sockets_.accept(handle);
  }

  [[nodiscard]] ReadResult socket_read(u8 handle, u8* output, u32 size) {
    return sockets_.read(handle, output, size);
  }

  [[nodiscard]] ReadResult socket_write(u8 handle, const u8* input,
                                        u32 size) {
    auto* value = sockets_.slot(handle);
    if (value == nullptr) {
      return {SocketResult::bad_handle, 0};
    }
    if ((value->state != SocketState::established &&
         value->state != SocketState::close_wait) ||
        value->send_closed) {
      return {SocketResult::not_connected, 0};
    }
    u32 written = 0;
    while (written < size) {
      const u32 count = size - written > transmit_segment_capacity
                            ? transmit_segment_capacity
                            : size - written;
      if (!queue_transmission(handle, *value, tcp_ack | tcp_psh,
                              input + written, count, value->send_next)) {
        return written == 0 ? ReadResult{SocketResult::no_space, 0}
                            : ReadResult{SocketResult::success, written};
      }
      value->acknowledgement_retries = 0;
      value->send_next += count;
      written += count;
    }
    return {SocketResult::success, written};
  }

  [[nodiscard]] SocketResult socket_shutdown(u8 handle, u32 how) {
    auto* value = sockets_.slot(handle);
    if (value == nullptr) {
      return SocketResult::bad_handle;
    }
    if (how > 2) {
      return SocketResult::invalid_argument;
    }
    if (value->state != SocketState::established &&
        value->state != SocketState::close_wait) {
      return SocketResult::not_connected;
    }
    if (how != 0 && !value->send_closed) {
      if (!send_segment(*value, tcp_fin | tcp_ack, nullptr, 0,
                        value->send_next)) {
        return SocketResult::no_space;
      }
      ++value->send_next;
      value->send_closed = true;
    }
    if (how != 1) {
      value->receive_size = 0;
      value->state = SocketState::close_wait;
    }
    return SocketResult::success;
  }

  [[nodiscard]] const SocketSlot* socket_slot(u8 handle) const {
    return sockets_.slot(handle);
  }

  [[nodiscard]] bool socket_readable(u8 handle) const {
    return sockets_.readable(handle);
  }

  [[nodiscard]] bool socket_writable(u8 handle) const {
    return sockets_.writable(handle);
  }

  [[nodiscard]] const char* tcp_table() {
    u32 cursor = 0;
    append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
           "  sl  local_address rem_address   st tx_queue rx_queue tr "
           "tm->when retrnsmt   uid  timeout inode\n");
    u32 row = 0;
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      const auto* value = sockets_.slot(handle);
      if (value == nullptr || value->type != abi::socket::Type::stream ||
          (value->state != SocketState::listening &&
           value->state != SocketState::syn_received &&
           value->state != SocketState::established &&
           value->state != SocketState::close_wait)) {
        continue;
      }
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), "   ");
      append_decimal(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
                     row++);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), ": ");
      append_ipv4(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
                  value->local.address);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), ":");
      append_hex(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
                 value->local.port, 4);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), " ");
      append_ipv4(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
                  value->remote.address);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), ":");
      append_hex(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
                 value->remote.port, 4);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), " ");
      const u32 state = value->state == SocketState::listening
                            ? 0x0a
                            : (value->state == SocketState::syn_received
                                   ? 0x03
                                   : (value->state == SocketState::close_wait
                                          ? 0x08
                                          : 0x01));
      append_hex(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), state,
                 2);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
             " 00000000:00000000 00:00000000 00000000   0        0 ");
      append_decimal(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_),
                     static_cast<u32>(handle) + 1);
      append(tcp_table_buffer_, cursor, sizeof(tcp_table_buffer_), "\n");
    }
    tcp_table_buffer_[cursor < sizeof(tcp_table_buffer_)
                          ? cursor
                          : sizeof(tcp_table_buffer_) - 1] = '\0';
    return tcp_table_buffer_;
  }

 private:
  [[nodiscard]] Reply handle(drivers::net::Frame frame) {
    if (const u32 size =
            make_arp_reply(frame.data, frame.size, state_.mac, state_.address);
        size != 0) {
      return drivers::net::transmit(frame.data, size) ? Reply::arp
                                                       : Reply::none;
    }
    if (const u32 size = make_icmp_echo_reply(frame.data, frame.size,
                                              state_.mac, state_.address);
        size != 0) {
      return drivers::net::transmit(frame.data, size) ? Reply::echo
                                                       : Reply::none;
    }
    handle_tcp(frame);
    return Reply::none;
  }

  [[nodiscard]] static bool unspecified(Ipv4Address address) {
    return address.octet[0] == 0 && address.octet[1] == 0 &&
           address.octet[2] == 0 && address.octet[3] == 0;
  }

  static void append(char* output, u32& cursor, u32 capacity,
                     const char* text) {
    while (*text != '\0' && cursor + 1 < capacity) {
      output[cursor++] = *text++;
    }
  }

  static void append_hex(char* output, u32& cursor, u32 capacity, u32 value,
                         u32 digits) {
    constexpr char hex[] = "0123456789ABCDEF";
    while (digits != 0 && cursor + 1 < capacity) {
      const u32 shift = (--digits) * 4;
      output[cursor++] = hex[(value >> shift) & 0x0f];
    }
  }

  static void append_decimal(char* output, u32& cursor, u32 capacity,
                             u32 value) {
    char digits[10]{};
    u32 count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (count != 0 && cursor + 1 < capacity) {
      output[cursor++] = digits[--count];
    }
  }

  static void append_ipv4(char* output, u32& cursor, u32 capacity,
                          Ipv4Address address) {
    for (u32 i = 4; i != 0; --i) {
      append_hex(output, cursor, capacity, address.octet[i - 1], 2);
    }
  }

  [[nodiscard]] bool send_segment(const SocketSlot& socket, u8 flags,
                                  const u8* payload, u32 payload_size,
                                  u32 sequence) {
    constexpr u32 headers = sizeof(EthernetHeader) + sizeof(Ipv4Header) +
                            sizeof(TcpHeader);
    if (payload_size > sizeof(transmit_buffer_) - headers) {
      return false;
    }
    auto& ethernet = *reinterpret_cast<EthernetHeader*>(transmit_buffer_);
    auto& ip = *reinterpret_cast<Ipv4Header*>(
        transmit_buffer_ + sizeof(EthernetHeader));
    auto& tcp = *reinterpret_cast<TcpHeader*>(
        transmit_buffer_ + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    ethernet = {};
    ip = {};
    tcp = {};
    copy_octets(ethernet.destination, socket.remote_mac.octet);
    copy_octets(ethernet.source, state_.mac.octet);
    ethernet.type = net16(0x0800);
    ip.version_ihl = 0x45;
    ip.total_length = net16(static_cast<u16>(sizeof(Ipv4Header) +
                                             sizeof(TcpHeader) + payload_size));
    ip.identification = net16(next_identification_++);
    ip.ttl = 64;
    ip.protocol = 6;
    copy_octets(ip.source, socket.local.address.octet);
    copy_octets(ip.destination, socket.remote.address.octet);
    tcp.source_port = net16(socket.local.port);
    tcp.destination_port = net16(socket.remote.port);
    tcp.sequence = net32(sequence);
    tcp.acknowledgement = net32(socket.receive_next);
    tcp.data_offset = 5 << 4;
    tcp.flags = flags;
    tcp.window = net16(static_cast<u16>(socket_receive_capacity -
                                        socket.receive_size));
    for (u32 i = 0; i < payload_size; ++i) {
      transmit_buffer_[headers + i] = payload[i];
    }
    tcp.checksum = net16(tcp_checksum(
        socket.local.address, socket.remote.address,
        reinterpret_cast<const u8*>(&tcp), sizeof(TcpHeader) + payload_size));
    ip.checksum = net16(internet_checksum(reinterpret_cast<const u8*>(&ip),
                                          sizeof(Ipv4Header)));
    return drivers::net::transmit(transmit_buffer_, headers + payload_size);
  }

  [[nodiscard]] bool queue_transmission(u8 handle, const SocketSlot& socket,
                                        u8 flags, const u8* payload,
                                        u32 payload_size, u32 sequence) {
    bool inserted = false;
    auto* pending = transmissions_.find_or_emplace({handle, sequence}, inserted);
    if (pending == nullptr || !inserted || payload_size > sizeof(pending->data)) {
      return false;
    }
    pending->size = static_cast<u16>(payload_size);
    pending->flags = flags;
    pending->attempts = 0;
    for (u32 i = 0; i < payload_size; ++i) {
      pending->data[i] = payload[i];
    }
    if (!send_segment(socket, flags, pending->data, payload_size, sequence)) {
      static_cast<void>(transmissions_.erase({handle, sequence}));
      return false;
    }
    pending->attempts = 1;
    pending->retry_at = poll_epoch_ + retransmission_poll_interval;
    return true;
  }

  void acknowledge_transmissions(u8 handle, u32 acknowledgement,
                                  u32 send_next) {
    if (static_cast<i32>(acknowledgement - send_next) > 0) {
      return;
    }
    transmissions_.erase_if([&](TransmitKey key) {
      if (key.handle != handle) {
        return false;
      }
      const auto* segment = transmissions_.find(key);
      if (segment == nullptr) {
        return false;
      }
      const u32 end = key.sequence + segment->size;
      return static_cast<i32>(acknowledgement - end) >= 0;
    });
  }

  void clear_transmissions(u8 handle) {
    transmissions_.erase_if(
        [handle](TransmitKey key) { return key.handle == handle; });
  }

  void flush_retransmission() {
    ++poll_epoch_;
    bool transmitted = false;
    transmissions_.for_each([&](TransmitKey key, TransmitSegment& pending) {
      if (transmitted || static_cast<i32>(poll_epoch_ - pending.retry_at) < 0) {
        return;
      }
      auto* socket = sockets_.slot(key.handle);
      if (socket == nullptr ||
          (socket->state != SocketState::established &&
           socket->state != SocketState::close_wait)) {
        return;
      }
      if (send_segment(*socket, pending.flags, pending.data, pending.size,
                       key.sequence)) {
        ++pending.attempts;
        pending.retry_at = poll_epoch_ + retransmission_poll_interval;
        transmitted = true;
      }
    });
  }

  void flush_acknowledgement(SocketSlot& socket) {
    if (socket.acknowledgement_retries == 0 ||
        (socket.state != SocketState::established &&
         socket.state != SocketState::close_wait)) {
      return;
    }
    if (send_segment(socket, tcp_ack, nullptr, 0, socket.send_next)) {
      --socket.acknowledgement_retries;
    }
  }

  void flush_acknowledgements() {
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      auto* socket = sockets_.slot(handle);
      if (socket != nullptr) {
        flush_acknowledgement(*socket);
      }
    }
  }

  void handle_tcp(drivers::net::Frame frame) {
    TcpView view{};
    if (!parse_tcp(frame.data, frame.size, state_.address, view)) {
      return;
    }
    const Endpoint local{state_.address, net16(view.tcp->destination_port)};
    const Endpoint remote{
        Ipv4Address{{view.ip->source[0], view.ip->source[1], view.ip->source[2],
                     view.ip->source[3]}},
        net16(view.tcp->source_port)};
    const u32 sequence = net32(view.tcp->sequence);
    const u32 acknowledgement = net32(view.tcp->acknowledgement);
    u8 connection = sockets_.connection(local, remote);

    if (connection == invalid_socket && (view.tcp->flags & tcp_syn) != 0 &&
        (view.tcp->flags & tcp_ack) == 0) {
      const u8 listening = sockets_.listener(local);
      if (listening == invalid_socket) {
        return;
      }
      const MacAddress remote_mac{{view.ethernet->source[0],
                                   view.ethernet->source[1],
                                   view.ethernet->source[2],
                                   view.ethernet->source[3],
                                   view.ethernet->source[4],
                                   view.ethernet->source[5]}};
      const u32 initial_sequence = next_sequence_;
      next_sequence_ += 0x10001;
      const auto started = sockets_.begin_connection(
          listening, local, remote, remote_mac, sequence, initial_sequence);
      if (started.result != SocketResult::success) {
        return;
      }
      connection = started.handle;
      const auto* value = sockets_.slot(connection);
      static_cast<void>(send_segment(*value, tcp_syn | tcp_ack, nullptr, 0,
                                     value->send_next - 1));
      return;
    }

    auto* value = sockets_.slot(connection);
    if (value == nullptr) {
      return;
    }
    if ((view.tcp->flags & tcp_rst) != 0) {
      clear_transmissions(connection);
      sockets_.reset(connection);
      return;
    }
    if (value->state == SocketState::syn_received) {
      if ((view.tcp->flags & tcp_syn) != 0) {
        static_cast<void>(send_segment(*value, tcp_syn | tcp_ack, nullptr, 0,
                                       value->send_next - 1));
        return;
      }
      if ((view.tcp->flags & tcp_ack) == 0 ||
          sockets_.establish(connection, acknowledgement) !=
              SocketResult::success) {
        return;
      }
    }
    if ((view.tcp->flags & tcp_ack) != 0) {
      acknowledge_transmissions(connection, acknowledgement,
                                 value->send_next);
    }
    const bool finish = (view.tcp->flags & tcp_fin) != 0;
    if (view.payload_size != 0 || finish) {
      static_cast<void>(sockets_.receive(connection, sequence, view.payload,
                                         view.payload_size, finish));
      // Send immediately, then retain one delayed retry. A failed immediate TX
      // therefore gets retried by poll(), while a successful one gets one safe
      // duplicate ACK during a long userspace compute interval.
      value->acknowledgement_retries = 2;
      flush_acknowledgement(*value);
    }
  }

  void record(Reply reply) {
    switch (reply) {
      case Reply::arp:
        if (!arp_replied_) {
          arp_replied_ = true;
          write_text("MIKOS:ARP_REPLY\n");
        }
        break;
      case Reply::echo:
        if (!echo_replied_) {
          echo_replied_ = true;
          write_text("MIKOS:ICMP_ECHO_REPLY\n");
        }
        break;
      case Reply::none:
        break;
    }
  }

  void announce_address() const {
    write_text("MIKOS:NET_IP 10.0.2.15\n");
    write_text("MIKOS:NET_MAC ");
    constexpr char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 6; ++i) {
      uart_put(hex[state_.mac.octet[i] >> 4]);
      uart_put(hex[state_.mac.octet[i] & 0x0f]);
      uart_put(i == 5 ? '\n' : ':');
    }
  }

  InterfaceState state_{};
  SocketTable sockets_{};
  container::FixedUnorderedMap<TransmitKey, TransmitSegment,
                               transmit_capacity, TransmitHash>
      transmissions_{};
  alignas(4) u8 transmit_buffer_[1536]{};
  char tcp_table_buffer_[2048]{};
  u32 next_sequence_{0x4d494b4f};
  u16 next_identification_{1};
  u32 poll_epoch_{};
  // Polling is the only clock available in the bootstrap stack. Keep the
  // interval long enough to avoid racing a normal delayed ACK, but short
  // enough to recover while a cycle-accurate userspace process is blocked.
  static constexpr u32 retransmission_poll_interval = 256;
  bool initialized_{};
  bool arp_replied_{};
  bool echo_replied_{};
};

BootstrapStack bootstrap_stack;

}  // namespace

bool initialize() { return bootstrap_stack.initialize(); }

bool boot_probe() { return bootstrap_stack.run(); }

void poll() { bootstrap_stack.poll(); }

InterfaceControlResult interface_ioctl(u32 request, Ifreq32& value) {
  return bootstrap_stack.ioctl(request, value);
}

OpenResult socket_open(abi::socket::Type type) {
  return bootstrap_stack.socket_open(type);
}

SocketResult socket_retain(u8 handle) {
  return bootstrap_stack.socket_retain(handle);
}

SocketResult socket_close(u8 handle) {
  return bootstrap_stack.socket_close(handle);
}

SocketResult socket_bind(u8 handle, Endpoint local) {
  return bootstrap_stack.socket_bind(handle, local);
}

SocketResult socket_listen(u8 handle, u32 backlog) {
  return bootstrap_stack.socket_listen(handle, backlog);
}

AcceptResult socket_accept(u8 handle) {
  return bootstrap_stack.socket_accept(handle);
}

ReadResult socket_read(u8 handle, u8* output, u32 size) {
  return bootstrap_stack.socket_read(handle, output, size);
}

ReadResult socket_write(u8 handle, const u8* input, u32 size) {
  return bootstrap_stack.socket_write(handle, input, size);
}

SocketResult socket_shutdown(u8 handle, u32 how) {
  return bootstrap_stack.socket_shutdown(handle, how);
}

const SocketSlot* socket_slot(u8 handle) {
  return bootstrap_stack.socket_slot(handle);
}

bool socket_readable(u8 handle) {
  return bootstrap_stack.socket_readable(handle);
}

bool socket_writable(u8 handle) {
  return bootstrap_stack.socket_writable(handle);
}

const char* tcp_table() { return bootstrap_stack.tcp_table(); }

}  // namespace mikos::network
