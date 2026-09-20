#include <drivers/net/net.hpp>
#include <mikos/arch.hpp>
#include <mikos/kernel.hpp>
#include <mikos/net/interface.hpp>
#include <mikos/net/tcp.hpp>
#include <mikos/net/tcp_table.hpp>

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
    now_ = arch::time_ticks();
    flush_acknowledgements();
    drivers::net::Frame frame{};
    if (drivers::net::receive(frame)) {
      record(handle(frame));
    }
    flush_timers();
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

  [[nodiscard]] SocketResult socket_retain(SocketHandle handle) {
    return sockets_.retain(handle);
  }

  [[nodiscard]] SocketResult socket_close(SocketHandle handle) {
    now_ = arch::time_ticks();
    auto* socket = sockets_.slot(handle);
    if (!socket) return SocketResult::bad_handle;
    if (socket->references == 1 && socket->fin_acked &&
        socket->receive_closed && socket->state != SocketState::time_wait) {
      clear_transmissions(handle);
      return sockets_.release(handle);
    }
    if (socket->references == 1 && is_connection(socket->state) &&
        socket->state != SocketState::reset &&
        socket->state != SocketState::syn_received) {
      socket->detached =
          true;  // Keep transport ownership until FIN/TIME-WAIT completes.
      request_fin(*socket);
      return SocketResult::success;
    }
    const bool final = socket->references == 1;
    const auto result = sockets_.release(handle);
    if (final) clear_transmissions(handle);
    return result;
  }

  [[nodiscard]] SocketResult socket_bind(SocketHandle handle, Endpoint local) {
    if (!unspecified(local.address) && local.address != state_.address) {
      return SocketResult::invalid_argument;
    }
    return sockets_.bind(handle, local);
  }

  [[nodiscard]] SocketResult socket_listen(SocketHandle handle, u32 backlog) {
    return sockets_.listen(handle, backlog);
  }

  [[nodiscard]] AcceptResult socket_accept(SocketHandle handle) {
    return sockets_.accept(handle);
  }

  [[nodiscard]] ReadResult socket_read(SocketHandle handle, u8* output,
                                       u32 size) {
    now_ = arch::time_ticks();
    const auto* before = sockets_.slot(handle);
    const bool was_time_wait =
        before && before->state == SocketState::time_wait;
    const auto result = sockets_.read(handle, output, size);
    if (result.size != 0) {
      if (auto* socket = sockets_.slot(handle)) {
        socket->acknowledgement_retries = 1;
        if (!was_time_wait && socket->state == SocketState::time_wait)
          socket->state_since = now_;
      }
    }
    return result;
  }

  [[nodiscard]] ReadResult socket_write(SocketHandle handle, const u8* input,
                                        u32 size) {
    now_ = arch::time_ticks();
    auto* socket = sockets_.slot(handle);
    if (!socket) return {SocketResult::bad_handle, 0};
    if (socket->state == SocketState::reset) return {socket->failure, 0};
    if (!sockets_.writable(handle)) return {SocketResult::not_connected, 0};
    if (size == 0) return {SocketResult::success, 0};
    if (socket->send_next == socket->send_unacknowledged &&
        now_ - socket->progress_at >= socket->rto)
      socket->congestion_window =
          std::min(socket->congestion_window, 4u * socket->peer_mss);
    const auto available = send_space(*socket);
    if (available == 0) {
      if (socket->peer_window == 0 && !socket->persist_waiting) {
        socket->persist_waiting = true;
        socket->progress_at = now_;
        socket->persist_delay = socket->rto;
        socket->persist_retry_at = now_ + socket->persist_delay;
      }
      return {SocketResult::would_block, 0};
    }
    u32 written = 0;
    while (written < size && written < available) {
      const u32 count =
          std::min({size - written, available - written,
                    transmit_segment_capacity, u32{socket->peer_mss}});
      if (!queue_transmission(handle, *socket, tcp_ack | tcp_psh,
                              std::span{input + written, count},
                              socket->send_next)) {
        return written == 0 ? ReadResult{SocketResult::would_block, 0}
                            : ReadResult{SocketResult::success, written};
      }
      if (socket->send_unacknowledged == socket->send_next)
        socket->progress_at = now_;
      socket->acknowledgement_retries = 0;
      socket->send_next += count;
      written += count;
    }
    return {SocketResult::success, written};
  }

  [[nodiscard]] SocketResult socket_shutdown(SocketHandle handle, u32 how) {
    now_ = arch::time_ticks();
    auto* socket = sockets_.slot(handle);
    if (!socket) return SocketResult::bad_handle;
    if (how > 2) return SocketResult::invalid_argument;
    if (!is_connection(socket->state) ||
        socket->state == SocketState::syn_received ||
        socket->state == SocketState::reset)
      return SocketResult::not_connected;
    if (how != 0) request_fin(*socket);
    if (how != 1) {
      socket->read_closed = true;
      socket->receive_buffer.clear();
      socket->acknowledgement_retries = 1;
    }
    return SocketResult::success;
  }

  [[nodiscard]] const SocketSlot* socket_slot(SocketHandle handle) const {
    return sockets_.slot(handle);
  }

  [[nodiscard]] bool socket_readable(SocketHandle handle) const {
    return sockets_.readable(handle);
  }

  [[nodiscard]] bool socket_writable(SocketHandle handle) const {
    const auto* socket = sockets_.slot(handle);
    return sockets_.writable(handle) && send_space(*socket) != 0 &&
           std::ranges::any_of(transmissions_,
                               [](const auto& slot) { return !slot; });
  }

  [[nodiscard]] const char* tcp_table() {
    format_tcp_table(sockets_.slots(), tcp_table_buffer_);
    return tcp_table_buffer_.data();
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
    return address == Ipv4Address{};
  }

  [[nodiscard]] bool send_segment(const SocketSlot& socket, u8 flags,
                                  std::span<const u8> payload, u32 sequence) {
    const std::array<u8, 8> syn_options{2, 4, 0x05, 0xb4, 1, 3, 3, 0};
    const auto options =
        (flags & tcp_syn) != 0
            ? std::span{syn_options}.first(socket.scale_negotiated ? 8 : 4)
            : std::span<const u8>{};
    const u32 tcp_header_size = sizeof(TcpHeader) + options.size();
    const u32 headers =
        sizeof(EthernetHeader) + sizeof(Ipv4Header) + tcp_header_size;
    if (payload.size() > transmit_buffer_.size() - headers) {
      return false;
    }
    EthernetHeader ethernet{};
    Ipv4Header ip{};
    TcpHeader tcp{};
    copy_octets(ethernet.destination, socket.remote_mac.octet);
    copy_octets(ethernet.source, state_.mac.octet);
    ethernet.type = net16(0x0800);
    ip.version_ihl = 0x45;
    ip.total_length = net16(static_cast<u16>(sizeof(Ipv4Header) +
                                             tcp_header_size + payload.size()));
    ip.identification = net16(next_identification_++);
    ip.ttl = 64;
    ip.protocol = 6;
    copy_octets(ip.source, socket.local.address.octet);
    copy_octets(ip.destination, socket.remote.address.octet);
    tcp.source_port = net16(socket.local.port);
    tcp.destination_port = net16(socket.remote.port);
    tcp.sequence = net32(sequence);
    tcp.acknowledgement = net32(socket.receive_next);
    tcp.data_offset = (tcp_header_size / 4) << 4;
    tcp.flags = flags;
    tcp.window = (flags & tcp_rst) != 0
                     ? 0
                     : net16(static_cast<u16>(socket_receive_capacity -
                                              socket.receive_buffer.size()));
    ip.checksum = net16(internet_checksum(
        std::bit_cast<std::array<u8, sizeof(Ipv4Header)>>(ip).data(),
        sizeof(ip)));
    std::ranges::copy(
        std::bit_cast<std::array<u8, sizeof(EthernetHeader)>>(ethernet),
        transmit_buffer_.begin());
    std::ranges::copy(std::bit_cast<std::array<u8, sizeof(Ipv4Header)>>(ip),
                      transmit_buffer_.begin() + sizeof(EthernetHeader));
    auto tcp_bytes = std::span{transmit_buffer_}.subspan(
        sizeof(EthernetHeader) + sizeof(Ipv4Header),
        tcp_header_size + payload.size());
    std::ranges::copy(std::bit_cast<std::array<u8, sizeof(TcpHeader)>>(tcp),
                      tcp_bytes.begin());
    std::ranges::copy(options, tcp_bytes.begin() + sizeof(TcpHeader));
    std::ranges::copy(payload, tcp_bytes.begin() + tcp_header_size);
    tcp.checksum =
        net16(tcp_checksum(socket.local.address, socket.remote.address,
                           tcp_bytes.data(), tcp_bytes.size()));
    std::ranges::copy(std::bit_cast<std::array<u8, sizeof(TcpHeader)>>(tcp),
                      tcp_bytes.begin());
    return drivers::net::transmit(transmit_buffer_.data(),
                                  headers + payload.size());
  }

  [[nodiscard]] bool queue_transmission(SocketHandle handle,
                                        const SocketSlot& socket, u8 flags,
                                        std::span<const u8> payload,
                                        u32 sequence) {
    const TransmitKey key{handle, sequence};
    if (payload.size() > transmit_segment_capacity ||
        std::ranges::any_of(transmissions_, [&](const auto& slot) {
          return slot && slot->first == key;
        }))
      return false;
    const auto free = std::ranges::find_if(
        transmissions_, [](const auto& slot) { return !slot; });
    if (free == transmissions_.end()) return false;
    auto* pending = &free->emplace().second;
    (*free)->first = key;
    pending->flags = flags;
    pending->attempts = 0;
    pending->data.assign_range(payload);
    if (!send_segment(socket, flags, pending->data, sequence)) {
      free->reset();
      return false;
    }
    pending->attempts = 1;
    pending->first_sent = now_;
    pending->delay = socket.rto;
    pending->retry_at = now_ + pending->delay;
    return true;
  }

  [[nodiscard]] static u32 send_space(const SocketSlot& socket) {
    const u32 flight = socket.send_next - socket.send_unacknowledged;
    const auto limit = std::min(socket.peer_window, socket.congestion_window);
    return flight < limit ? limit - flight : 0;
  }

  void acknowledge_transmissions(SocketHandle handle, SocketSlot& socket,
                                 u32 ack) {
    if (static_cast<i32>(ack - socket.send_unacknowledged) <= 0) return;
    const auto newly_acked = ack - socket.send_unacknowledged;
    socket.duplicate_acks = 0;
    if (socket.fast_recovery) {
      socket.congestion_window = socket.slow_start_threshold;
      socket.fast_recovery = false;
    } else if (socket.congestion_window < socket.slow_start_threshold) {
      socket.congestion_window += std::min<u32>(newly_acked, socket.peer_mss);
    } else {
      socket.congestion_credit += newly_acked;
      if (socket.congestion_credit >= socket.congestion_window) {
        socket.congestion_credit -= socket.congestion_window;
        socket.congestion_window += socket.peer_mss;
      }
    }
    socket.congestion_window =
        std::min(socket.congestion_window,
                 transmit_capacity * transmit_segment_capacity);
    bool retransmitted = false;
    for (const auto& pending : transmissions_)
      if (pending && pending->first.handle == handle &&
          pending->second.attempts > 1)
        retransmitted = true;
    bool sampled = false;
    for (auto& slot : transmissions_) {
      if (!slot || slot->first.handle != handle) continue;
      auto& [key, pending] = *slot;
      const u32 acknowledged = ack - key.sequence;
      if (static_cast<i32>(acknowledged) <= 0) continue;
      if (acknowledged >= pending.data.size()) {
        if (!retransmitted && !sampled) {
          update_rtt(socket, now_ - pending.first_sent);
          sampled = true;
        }
        slot.reset();
      } else {
        pending.data.erase(pending.data.begin(),
                           pending.data.begin() + acknowledged);
        key.sequence = ack;
      }
    }
    socket.send_unacknowledged = ack;
    socket.progress_at = now_;
    // Restart the oldest outstanding segment's timer after genuine progress.
    for (auto& slot : transmissions_) {
      if (slot && slot->first.handle == handle) {
        slot->second.delay = socket.rto;
        slot->second.retry_at = now_ + socket.rto;
      }
    }
  }

  static void update_rtt(SocketSlot& socket, u64 sample) {
    if (!socket.rtt_measured) {
      socket.smoothed_rtt = sample;
      socket.rtt_variance = sample / 2;
      socket.rtt_measured = true;
    } else {
      const auto error = socket.smoothed_rtt > sample
                             ? socket.smoothed_rtt - sample
                             : sample - socket.smoothed_rtt;
      socket.rtt_variance = (3 * socket.rtt_variance + error) / 4;
      socket.smoothed_rtt = (7 * socket.smoothed_rtt + sample) / 8;
    }
    socket.rto = std::clamp(
        socket.smoothed_rtt + std::max<u64>(1, 4 * socket.rtt_variance),
        tcp_initial_rto, tcp_max_rto);
  }

  void clear_transmissions(SocketHandle handle) {
    for (auto& slot : transmissions_)
      if (slot && slot->first.handle == handle) slot.reset();
  }

  void fail_connection(SocketHandle handle) {
    auto* socket = sockets_.slot(handle);
    if (!socket) return;
    clear_transmissions(handle);
    if (socket->state != SocketState::syn_received)
      static_cast<void>(
          send_segment(*socket, tcp_rst | tcp_ack, {}, socket->send_next));
    if (!socket->accepted || socket->detached)
      static_cast<void>(sockets_.release(handle));
    else
      sockets_.reset(handle, SocketResult::timed_out);
  }

  void request_fin(SocketSlot& socket) {
    if (socket.send_closed) return;
    socket.send_closed = true;
    socket.active_close = !socket.receive_closed;
    socket.state =
        socket.active_close ? SocketState::fin_wait_1 : SocketState::last_ack;
    socket.state_since = now_;
    socket.control_retry_at = now_;
    socket.control_delay = socket.rto;
    send_fin(socket);
  }

  void send_fin(SocketSlot& socket) {
    // All data survives close(), and is acknowledged before FIN is emitted.
    if (!socket.fin_sent && (socket.send_unacknowledged != socket.send_next ||
                             socket.peer_window == 0))
      return;
    const auto sequence =
        socket.fin_sent ? socket.send_next - 1 : socket.send_next;
    if (send_segment(socket, tcp_fin | tcp_ack, {}, sequence)) {
      if (!socket.fin_sent) {
        ++socket.send_next;
        socket.fin_sent = true;
      }
      socket.control_retry_at = now_ + socket.control_delay;
      socket.control_delay = backoff(socket.control_delay);
    } else
      socket.control_retry_at = now_ + tcp_driver_retry;
  }

  void enter_time_wait(SocketSlot& socket) {
    socket.state = SocketState::time_wait;
    socket.state_since = now_;
  }

  void flush_timers() {
    for (auto& socket : sockets_.slots()) {
      if (!is_connection(socket.state) || socket.state == SocketState::reset)
        continue;
      const auto handle = sockets_.handle_of(socket);
      if (socket.state == SocketState::time_wait) {
        if (now_ - socket.state_since >= tcp_time_wait) {
          if (socket.detached)
            static_cast<void>(sockets_.release(handle));
          else
            socket.state = SocketState::closed;
        }
        continue;
      }
      if (socket.state == SocketState::syn_received) {
        if (now_ - socket.opened_at >= tcp_syn_timeout) {
          fail_connection(handle);
          continue;
        }
        if (deadline_reached(now_, socket.control_retry_at)) {
          if (send_segment(socket, tcp_syn | tcp_ack, {},
                           socket.send_next - 1)) {
            socket.congestion_window = socket.peer_mss;
            socket.rto = 3 * tcp_second;
            socket.control_retry_at = now_ + socket.control_delay;
            socket.control_delay = backoff(socket.control_delay);
          } else
            socket.control_retry_at = now_ + tcp_driver_retry;
        }
        continue;
      }
      if ((socket.send_unacknowledged != socket.send_next ||
           socket.persist_waiting) &&
          now_ - socket.progress_at >= tcp_user_timeout) {
        fail_connection(handle);
        continue;
      }
      if (socket.send_closed && !socket.fin_acked &&
          now_ - socket.state_since >= tcp_user_timeout) {
        fail_connection(handle);
        continue;
      }
      if (socket.state == SocketState::fin_wait_2 && socket.detached &&
          now_ - socket.state_since >= tcp_fin_wait_timeout) {
        fail_connection(handle);
        continue;
      }
      if (socket.send_closed && !socket.fin_acked &&
          deadline_reached(now_, socket.control_retry_at))
        send_fin(socket);
      if (socket.persist_waiting && socket.peer_window == 0 &&
          deadline_reached(now_, socket.persist_retry_at)) {
        // No new application byte is committed by a window query. A stale
        // sequence elicits an ACK carrying the peer's current window.
        if (send_segment(socket, tcp_ack, {}, socket.send_unacknowledged - 1)) {
          socket.persist_delay = backoff(socket.persist_delay);
          socket.persist_retry_at = now_ + socket.persist_delay;
        } else
          socket.persist_retry_at = now_ + tcp_driver_retry;
      }
    }
  }

  void flush_retransmission() {
    for (std::size_t checked = 0; checked < transmissions_.size(); ++checked) {
      const auto index = (retry_cursor_ + checked) % transmissions_.size();
      auto& slot = transmissions_[index];
      if (!slot) continue;
      auto& [key, pending] = *slot;
      auto* socket = sockets_.slot(key.handle);
      if (!socket || socket->state == SocketState::reset) {
        slot.reset();
        continue;
      }
      // Only the oldest data in each flow is retransmitted. Sequence numbers
      // from independent connections must never be ordered against each other.
      if (key.sequence != socket->send_unacknowledged ||
          !deadline_reached(now_, pending.retry_at))
        continue;
      const auto count = std::min<std::size_t>(
          pending.data.size(), std::max<u32>(1, socket->peer_window));
      if (send_segment(*socket, pending.flags,
                       std::span{pending.data}.first(count), key.sequence)) {
        pending.attempts = std::min<unsigned>(pending.attempts + 1, 255);
        if (!pending.fast_retry && socket->peer_window != 0) {
          socket->slow_start_threshold = std::max<u32>(
              (socket->send_next - socket->send_unacknowledged) / 2,
              2u * socket->peer_mss);
          socket->congestion_window = socket->peer_mss;
          socket->congestion_credit = 0;
          socket->fast_recovery = false;
          socket->duplicate_acks = 0;
        }
        if (!pending.fast_retry) pending.delay = backoff(pending.delay);
        pending.fast_retry = false;
        pending.retry_at = now_ + pending.delay;
      } else
        pending.retry_at = now_ + tcp_driver_retry;
      retry_cursor_ = (index + 1) % transmissions_.size();
      return;  // Bound timer work and NIC pressure per poll.
    }
  }

  void flush_acknowledgement(SocketSlot& socket) {
    if (socket.acknowledgement_retries == 0 ||
        (!is_connection(socket.state) ||
         socket.state == SocketState::syn_received ||
         socket.state == SocketState::reset)) {
      return;
    }
    if (send_segment(socket, tcp_ack, {}, socket.send_next)) {
      --socket.acknowledgement_retries;
    }
  }

  void flush_acknowledgements() {
    for (auto& socket : sockets_.slots()) {
      flush_acknowledgement(socket);
    }
  }

  void handle_tcp(drivers::net::Frame frame) {
    const auto packet =
        parse_tcp(std::span{frame.data, frame.size}, state_.address);
    if (!packet) return;
    const auto& view = *packet;
    const Endpoint local{state_.address, net16(view.tcp.destination_port)};
    const Endpoint remote{Ipv4Address{{view.ip.source[0], view.ip.source[1],
                                       view.ip.source[2], view.ip.source[3]}},
                          net16(view.tcp.source_port)};
    const u32 sequence = net32(view.tcp.sequence);
    const u32 ack = net32(view.tcp.acknowledgement);
    const auto flags = view.tcp.flags;
    auto connection = sockets_.connection(local, remote);
    if (connection == invalid_socket) {
      if ((flags & tcp_rst) != 0) return;
      const auto listening = sockets_.listener(local);
      const MacAddress remote_mac{
          {view.ethernet.source[0], view.ethernet.source[1],
           view.ethernet.source[2], view.ethernet.source[3],
           view.ethernet.source[4], view.ethernet.source[5]}};
      if (listening == invalid_socket || (flags & tcp_ack) != 0) {
        if (!allow_control_reply()) return;
        SocketSlot rejected{};
        rejected.local = local;
        rejected.remote = remote;
        rejected.remote_mac = remote_mac;
        if ((flags & tcp_ack) != 0) {
          static_cast<void>(send_segment(rejected, tcp_rst, {}, ack));
        } else {
          rejected.receive_next = sequence + view.payload.size() +
                                  ((flags & tcp_syn) != 0) +
                                  ((flags & tcp_fin) != 0);
          static_cast<void>(send_segment(rejected, tcp_rst | tcp_ack, {}, 0));
        }
        return;
      }
      if ((flags & (tcp_syn | tcp_ack | tcp_fin)) != tcp_syn) return;
      const auto started = sockets_.begin_connection(
          listening, local, remote, remote_mac, sequence, next_sequence_);
      if (started.result != SocketResult::success) return;
      next_sequence_ += 0x10001;
      auto& socket = *sockets_.slot(started.handle);
      socket.opened_at = socket.progress_at = now_;
      socket.peer_window =
          net16(view.tcp.window);  // SYN windows are never scaled.
      socket.peer_mss =
          std::min<u32>(view.options.maximum_segment_size.value_or(536),
                        transmit_segment_capacity);
      socket.congestion_window = 4u * socket.peer_mss;
      socket.scale_negotiated = view.options.window_scale.has_value();
      socket.peer_scale = view.options.window_scale.value_or(0);
      const bool sent =
          send_segment(socket, tcp_syn | tcp_ack, {}, socket.send_next - 1);
      socket.control_retry_at = now_ + (sent ? socket.rto : tcp_driver_retry);
      socket.control_delay = sent ? backoff(socket.rto) : socket.rto;
      return;
    }
    auto* socket = sockets_.slot(connection);
    if (!socket || socket->state == SocketState::reset) return;
    if ((flags & tcp_rst) != 0) {
      // A delayed reset must not remove the protection against tuple reuse.
      if (socket->state == SocketState::time_wait) return;
      if (sequence == socket->receive_next) {
        clear_transmissions(connection);
        if (socket->detached || !socket->accepted)
          static_cast<void>(sockets_.release(connection));
        else
          sockets_.reset(connection);
      } else if (static_cast<i32>(sequence - socket->receive_next) > 0 &&
                 sequence - socket->receive_next < socket_receive_capacity)
        challenge_ack(*socket);
      return;
    }
    if (socket->state == SocketState::time_wait) {
      if ((flags & tcp_fin) != 0 &&
          sequence + view.payload.size() + 1 == socket->receive_next) {
        socket->state_since = now_;
        challenge_ack(*socket);
      }
      return;
    }
    if (socket->state == SocketState::syn_received) {
      if ((flags & (tcp_syn | tcp_fin | tcp_ack)) == tcp_syn &&
          sequence + 1 == socket->receive_next) {
        static_cast<void>(send_segment(*socket, tcp_syn | tcp_ack, {},
                                       socket->send_next - 1));
        return;
      }
      if ((flags & (tcp_ack | tcp_syn)) != tcp_ack ||
          sequence != socket->receive_next ||
          sockets_.establish(connection, ack) != SocketResult::success)
        return;
    } else if ((flags & tcp_syn) != 0) {
      challenge_ack(*socket);
      return;
    }
    if ((flags & tcp_ack) == 0) return;
    const i32 distance = static_cast<i32>(sequence - socket->receive_next);
    const u32 length = view.payload.size() + ((flags & tcp_fin) != 0);
    const u32 window = socket_receive_capacity - socket->receive_buffer.size();
    const bool acceptable =
        length == 0 ? (distance == 0 ||
                       (distance > 0 && static_cast<u32>(distance) < window))
                    : (window != 0 && distance < static_cast<i32>(window) &&
                       static_cast<__INT64_TYPE__>(distance) + length > 0);
    if (!acceptable) {
      challenge_ack(*socket);
      return;
    }
    if (static_cast<i32>(ack - socket->send_next) > 0) {
      challenge_ack(*socket);
      return;
    }
    if (static_cast<i32>(ack - socket->send_unacknowledged) >= 0) {
      const auto advertised = u32{net16(view.tcp.window)} << socket->peer_scale;
      if (ack == socket->send_unacknowledged && socket->send_next != ack &&
          length == 0 && advertised == socket->peer_window &&
          !socket->fin_sent && advertised != 0) {
        if (socket->duplicate_acks < 255) ++socket->duplicate_acks;
        if (socket->duplicate_acks == 3) {
          socket->slow_start_threshold = std::max<u32>(
              (socket->send_next - ack) / 2, 2u * socket->peer_mss);
          socket->congestion_window =
              socket->slow_start_threshold + 3u * socket->peer_mss;
          socket->fast_recovery = true;
          for (auto& pending : transmissions_) {
            if (pending && pending->first.handle == connection &&
                pending->first.sequence == ack) {
              pending->second.fast_retry = true;
              pending->second.retry_at = now_;
            }
          }
        }
      } else
        socket->duplicate_acks = 0;
      acknowledge_transmissions(connection, *socket, ack);
      if (static_cast<i32>(sequence - socket->window_sequence) > 0 ||
          (sequence == socket->window_sequence &&
           static_cast<i32>(ack - socket->window_acknowledgement) >= 0)) {
        socket->peer_window = u32{net16(view.tcp.window)} << socket->peer_scale;
        socket->window_sequence = sequence;
        socket->window_acknowledgement = ack;
        if (socket->peer_window == 0 &&
            socket->send_next != socket->send_unacknowledged &&
            !socket->persist_waiting) {
          socket->persist_waiting = true;
          socket->persist_delay = socket->rto;
          socket->persist_retry_at = now_ + socket->persist_delay;
        }
        if (socket->persist_waiting) {
          socket->progress_at =
              now_;  // A responsive zero-window peer stays alive.
          if (socket->peer_window != 0) socket->persist_waiting = false;
        }
      }
    }
    if (socket->fin_sent && ack == socket->send_next) {
      socket->fin_acked = true;
      if (socket->receive_closed) {
        if (socket->active_close)
          enter_time_wait(*socket);
        else if (socket->detached) {
          static_cast<void>(sockets_.release(connection));
          return;
        }
      } else if (socket->state == SocketState::fin_wait_1) {
        socket->state = SocketState::fin_wait_2;
        socket->state_since = now_;
      }
    }
    const bool finish = (flags & tcp_fin) != 0;
    if (!view.payload.empty() || finish) {
      const bool was_closed = socket->receive_closed;
      static_cast<void>(sockets_.receive(connection, sequence,
                                         view.payload.data(),
                                         view.payload.size(), finish));
      if (!was_closed && socket->receive_closed && socket->fin_acked)
        enter_time_wait(*socket);
      if (socket->read_closed) socket->receive_buffer.clear();
      socket->acknowledgement_retries = 1;
      flush_acknowledgement(*socket);
    }
    if (socket->send_closed && !socket->fin_sent) send_fin(*socket);
  }

  [[nodiscard]] bool allow_control_reply() {
    if (now_ - challenge_epoch_ >= tcp_second) {
      challenge_epoch_ = now_;
      challenges_ = 0;
    }
    if (challenges_ >= 100) return false;
    ++challenges_;
    return true;
  }

  void challenge_ack(SocketSlot& socket) {
    if (!allow_control_reply()) return;
    socket.acknowledgement_retries = 1;
    flush_acknowledgement(socket);
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
  std::array<std::optional<std::pair<TransmitKey, TransmitSegment>>,
             transmit_capacity>
      transmissions_{};
  alignas(4) std::array<u8, 1536> transmit_buffer_{};
  TcpTableText tcp_table_buffer_{};
  u32 next_sequence_{0x4d494b4f};
  u16 next_identification_{1};
  u64 now_{};
  u64 challenge_epoch_{};
  u32 challenges_{};
  std::size_t retry_cursor_{};
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

SocketResult socket_retain(SocketHandle handle) {
  return bootstrap_stack.socket_retain(handle);
}

SocketResult socket_close(SocketHandle handle) {
  return bootstrap_stack.socket_close(handle);
}

SocketResult socket_bind(SocketHandle handle, Endpoint local) {
  return bootstrap_stack.socket_bind(handle, local);
}

SocketResult socket_listen(SocketHandle handle, u32 backlog) {
  return bootstrap_stack.socket_listen(handle, backlog);
}

AcceptResult socket_accept(SocketHandle handle) {
  return bootstrap_stack.socket_accept(handle);
}

ReadResult socket_read(SocketHandle handle, u8* output, u32 size) {
  return bootstrap_stack.socket_read(handle, output, size);
}

ReadResult socket_write(SocketHandle handle, const u8* input, u32 size) {
  return bootstrap_stack.socket_write(handle, input, size);
}

SocketResult socket_shutdown(SocketHandle handle, u32 how) {
  return bootstrap_stack.socket_shutdown(handle, how);
}

const SocketSlot* socket_slot(SocketHandle handle) {
  return bootstrap_stack.socket_slot(handle);
}

bool socket_readable(SocketHandle handle) {
  return bootstrap_stack.socket_readable(handle);
}

bool socket_writable(SocketHandle handle) {
  return bootstrap_stack.socket_writable(handle);
}

const char* tcp_table() { return bootstrap_stack.tcp_table(); }

}  // namespace mikos::network
