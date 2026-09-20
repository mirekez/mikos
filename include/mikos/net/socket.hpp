#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <mikos/abi/socket.hpp>
#include <mikos/container/inplace_vector.hpp>
#include <mikos/net/ethernet.hpp>
#include <mikos/net/timing.hpp>
#include <optional>
#include <span>

namespace mikos::network {

inline constexpr u32 socket_capacity = 13;
inline constexpr u32 socket_receive_capacity = 4096;
inline constexpr u32 listen_backlog_capacity = 4;
inline constexpr u32 reassembly_capacity = 16;
inline constexpr u32 reassembly_segment_capacity = 1500;
inline constexpr u32 transmit_capacity = 16;
inline constexpr u32 transmit_segment_capacity = 1024;
// An internal socket identity, distinct from a userspace file descriptor.
// Only SocketTable can create an identity for a slot; default construction
// produces the invalid handle. Copies remain cheap for process snapshots.
class SocketHandle {
 public:
  constexpr SocketHandle() = default;

  [[nodiscard]] constexpr std::size_t index() const { return index_; }
  [[nodiscard]] constexpr bool operator==(const SocketHandle&) const = default;

 private:
  friend class SocketTable;
  static constexpr auto invalid_index = std::numeric_limits<u8>::max();
  static_assert(socket_capacity <= invalid_index);

  explicit constexpr SocketHandle(std::size_t index)
      : index_{static_cast<u8>(index)} {}

  u8 index_{invalid_index};
};

inline constexpr SocketHandle invalid_socket{};

enum class SocketState : u8 {
  free,
  control,
  created,
  bound,
  listening,
  syn_received,
  established,
  close_wait,
  fin_wait_1,
  fin_wait_2,
  closing,
  last_ack,
  time_wait,
  closed,
  reset,
};

enum class SocketResult : u8 {
  success,
  bad_handle,
  wrong_type,
  invalid_argument,
  address_in_use,
  not_bound,
  not_listening,
  would_block,
  not_connected,
  no_space,
  reset,
  end_of_file,
  timed_out,
};

struct Endpoint {
  Ipv4Address address{};
  u16 port{};

  [[nodiscard]] constexpr bool operator==(const Endpoint&) const = default;
};

[[nodiscard]] constexpr bool is_connection(SocketState state) {
  switch (state) {
    case SocketState::syn_received:
    case SocketState::established:
    case SocketState::close_wait:
    case SocketState::fin_wait_1:
    case SocketState::fin_wait_2:
    case SocketState::closing:
    case SocketState::last_ack:
    case SocketState::time_wait:
    case SocketState::reset:
      return true;
    default:
      return false;
  }
}

struct SocketSlot {
  SocketState state{SocketState::free};
  abi::socket::Type type{abi::socket::Type::datagram};
  Endpoint local{};
  Endpoint remote{};
  MacAddress remote_mac{};
  u32 send_next{};
  u32 send_unacknowledged{};
  u32 peer_window{65535};
  u32 congestion_window{4 * 536};
  u32 slow_start_threshold{65535};
  u32 congestion_credit{};
  u8 duplicate_acks{};
  bool fast_recovery{};
  u32 window_sequence{};
  u32 window_acknowledgement{};
  u16 peer_mss{536};
  u8 peer_scale{};
  bool scale_negotiated{};
  bool receive_closed{};
  bool read_closed{};
  bool detached{};
  bool fin_sent{};
  bool fin_acked{};
  bool active_close{};
  bool persist_waiting{};
  bool rtt_measured{};
  SocketResult failure{SocketResult::reset};
  u64 opened_at{};
  u64 progress_at{};
  u64 control_retry_at{};
  u64 control_delay{tcp_initial_rto};
  u64 persist_retry_at{};
  u64 persist_delay{tcp_initial_rto};
  u64 state_since{};
  u64 rto{tcp_initial_rto};
  u64 smoothed_rtt{};
  u64 rtt_variance{};
  u32 receive_next{};
  u16 references{};
  SocketHandle listener{invalid_socket};
  u8 backlog{};
  // Number of pending ACK send attempts. A successful immediate send clears
  // this; a busy TX descriptor leaves it set for the next poll.
  u8 acknowledgement_retries{};
  bool accepted{};
  bool send_closed{};
  inplace_vector<u8, socket_receive_capacity> receive_buffer{};
};

struct OpenResult {
  SocketResult result{SocketResult::no_space};
  SocketHandle handle{invalid_socket};
};

struct AcceptResult {
  SocketResult result{SocketResult::would_block};
  SocketHandle handle{invalid_socket};
  Endpoint peer{};
};

struct ReadResult {
  SocketResult result{SocketResult::would_block};
  u32 size{};
};

struct ReassemblyKey {
  SocketHandle handle{invalid_socket};
  u32 sequence{};

  [[nodiscard]] constexpr bool operator==(const ReassemblyKey&) const = default;
};

struct ReassemblySegment {
  bool finish{};
  inplace_vector<u8, reassembly_segment_capacity> data{};
};

struct TransmitKey {
  SocketHandle handle{invalid_socket};
  u32 sequence{};

  [[nodiscard]] constexpr bool operator==(const TransmitKey&) const = default;
};

struct TransmitSegment {
  u64 retry_at{};
  u64 delay{tcp_initial_rto};
  u64 first_sent{};
  bool fast_retry{};
  u8 flags{};
  u8 attempts{};
  inplace_vector<u8, transmit_segment_capacity> data{};
};

class SocketTable {
 public:
  [[nodiscard]] OpenResult open(abi::socket::Type type) {
    const SocketHandle handle = allocate();
    if (handle == invalid_socket) {
      return {};
    }
    auto& value = slots_[handle.index()];
    value.type = type;
    value.state = type == abi::socket::Type::datagram ? SocketState::control
                                                      : SocketState::created;
    value.references = 1;
    return {SocketResult::success, handle};
  }

  [[nodiscard]] SocketResult retain(SocketHandle handle) {
    auto* value = slot(handle);
    if (value == nullptr || value->references == 0xffff) {
      return SocketResult::bad_handle;
    }
    ++value->references;
    return SocketResult::success;
  }

  [[nodiscard]] SocketResult release(SocketHandle handle) {
    auto* value = slot(handle);
    if (value == nullptr || value->references == 0) {
      return SocketResult::bad_handle;
    }
    if (--value->references != 0) {
      return SocketResult::success;
    }
    if (value->state == SocketState::listening) {
      for (std::size_t index = 0; index < slots_.size(); ++index) {
        const SocketHandle candidate_handle{index};
        auto& candidate = slots_[index];
        if (candidate.listener == handle && !candidate.accepted) {
          clear_reassembly(candidate_handle);
          candidate = {};
        }
      }
    }
    clear_reassembly(handle);
    *value = {};
    return SocketResult::success;
  }

  [[nodiscard]] SocketResult bind(SocketHandle handle, Endpoint local) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return SocketResult::bad_handle;
    }
    if (value->type != abi::socket::Type::stream) {
      return SocketResult::wrong_type;
    }
    if (value->state != SocketState::created) {
      return SocketResult::invalid_argument;
    }
    if (local.port == 0) {
      local.port = allocate_ephemeral_port(local.address);
      if (local.port == 0) {
        return SocketResult::no_space;
      }
    }
    if (port_conflicts(handle, local)) {
      return SocketResult::address_in_use;
    }
    value->local = local;
    value->state = SocketState::bound;
    return SocketResult::success;
  }

  [[nodiscard]] SocketResult listen(SocketHandle handle, u32 backlog) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return SocketResult::bad_handle;
    }
    if (value->type != abi::socket::Type::stream) {
      return SocketResult::wrong_type;
    }
    if (value->state != SocketState::bound &&
        value->state != SocketState::listening) {
      return SocketResult::not_bound;
    }
    value->backlog =
        static_cast<u8>(std::clamp(backlog, u32{1}, listen_backlog_capacity));
    value->state = SocketState::listening;
    return SocketResult::success;
  }

  [[nodiscard]] SocketHandle listener(Endpoint local) const {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      const SocketHandle handle{index};
      const auto& value = slots_[index];
      if (value.state == SocketState::listening &&
          value.local.port == local.port &&
          (unspecified(value.local.address) ||
           value.local.address == local.address)) {
        return handle;
      }
    }
    return invalid_socket;
  }

  [[nodiscard]] SocketHandle connection(Endpoint local, Endpoint remote) const {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      const SocketHandle handle{index};
      const auto& value = slots_[index];
      if (is_connection(value.state) && value.local == local &&
          value.remote == remote) {
        return handle;
      }
    }
    return invalid_socket;
  }

  [[nodiscard]] OpenResult begin_connection(SocketHandle listener_handle,
                                            Endpoint local, Endpoint remote,
                                            MacAddress remote_mac,
                                            u32 remote_sequence,
                                            u32 local_sequence) {
    auto* listening = slot(listener_handle);
    if (listening == nullptr || listening->state != SocketState::listening) {
      return {SocketResult::not_listening, invalid_socket};
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      const SocketHandle handle{index};
      const auto& candidate = slots_[index];
      if (candidate.listener == listener_handle && candidate.remote == remote &&
          !candidate.accepted) {
        return {SocketResult::success, handle};
      }
    }
    if (pending(listener_handle) >= listening->backlog) {
      return {SocketResult::no_space, invalid_socket};
    }
    const SocketHandle handle = allocate();
    if (handle == invalid_socket) {
      return {};
    }
    auto& value = slots_[handle.index()];
    value.state = SocketState::syn_received;
    value.type = abi::socket::Type::stream;
    value.local = local;
    value.remote = remote;
    value.remote_mac = remote_mac;
    value.send_next = local_sequence + 1;
    value.send_unacknowledged = local_sequence;
    value.window_sequence = remote_sequence;
    value.window_acknowledgement = local_sequence;
    value.receive_next = remote_sequence + 1;
    value.references = 1;
    value.listener = listener_handle;
    return {SocketResult::success, handle};
  }

  [[nodiscard]] SocketResult establish(SocketHandle handle,
                                       u32 acknowledgement) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return SocketResult::bad_handle;
    }
    if (value->state == SocketState::established ||
        value->state == SocketState::close_wait) {
      return SocketResult::success;
    }
    if (value->state != SocketState::syn_received ||
        acknowledgement != value->send_next) {
      return SocketResult::invalid_argument;
    }
    value->state = SocketState::established;
    value->send_unacknowledged = acknowledgement;
    return SocketResult::success;
  }

  [[nodiscard]] AcceptResult accept(SocketHandle listener_handle) {
    const auto* listening = slot(listener_handle);
    if (listening == nullptr) {
      return {SocketResult::bad_handle, invalid_socket, {}};
    }
    if (listening->state != SocketState::listening) {
      return {SocketResult::not_listening, invalid_socket, {}};
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      const SocketHandle handle{index};
      auto& candidate = slots_[index];
      if (candidate.listener == listener_handle && !candidate.accepted &&
          (candidate.state == SocketState::established ||
           candidate.state == SocketState::close_wait ||
           candidate.state == SocketState::reset)) {
        candidate.accepted = true;
        return {SocketResult::success, handle, candidate.remote};
      }
    }
    return {SocketResult::would_block, invalid_socket, {}};
  }

  [[nodiscard]] ReadResult receive(SocketHandle handle, u32 sequence,
                                   const u8* data, u32 size, bool finish) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return {SocketResult::bad_handle, 0};
    }
    if (!is_connection(value->state) ||
        value->state == SocketState::syn_received ||
        value->state == SocketState::reset) {
      return {value->state == SocketState::reset ? SocketResult::reset
                                                 : SocketResult::not_connected,
              0};
    }
    if (value->receive_closed) return {SocketResult::success, 0};
    i32 distance = static_cast<i32>(sequence - value->receive_next);
    if (distance < 0) {
      // Do not discard an entire retransmission merely because its prefix was
      // acknowledged. This occurs when the receive buffer accepted only part
      // of a segment: trim the overlap and retain the unseen tail (or FIN).
      const u32 acknowledged = value->receive_next - sequence;
      if (acknowledged > size || (acknowledged == size && !finish)) {
        return {SocketResult::success, 0};
      }
      data += acknowledged;
      size -= acknowledged;
      sequence += acknowledged;
      distance = 0;
    }
    const auto window = static_cast<u32>(value->receive_buffer.capacity() -
                                         value->receive_buffer.size());
    if (static_cast<u32>(distance) >= window && (size != 0 || finish)) {
      return {SocketResult::success, 0};
    }
    const u32 available = window - static_cast<u32>(distance);
    if (size >= available) finish = false;
    size = std::min(size, available);
    if (distance > 0) {
      if (size == 0 && !finish) return {SocketResult::success, 0};
      return {store_reassembly(handle, sequence, data, size, finish), 0};
    }
    u32 total = 0;
    const auto immediate = append_received(*value, data, size, finish);
    total += immediate.size;
    if (immediate.result != SocketResult::success || immediate.size != size) {
      return {immediate.result, total};
    }
    const auto drained = drain_reassembly(handle, *value);
    total += drained.size;
    if (drained.result != SocketResult::success) {
      return {drained.result, total};
    }
    return {SocketResult::success, total};
  }

  [[nodiscard]] ReadResult append_received(SocketSlot& value, const u8* data,
                                           u32 size, bool finish) {
    const auto accepted = static_cast<u32>(std::min<usize>(
        size, value.receive_buffer.capacity() - value.receive_buffer.size()));
    value.receive_buffer.append_range(std::span{data, accepted});
    value.receive_next += accepted;
    if (finish && accepted == size) {
      ++value.receive_next;
      value.receive_closed = true;
      if (value.state == SocketState::fin_wait_1)
        value.state = SocketState::closing;
      else if (value.state == SocketState::fin_wait_2)
        value.state = SocketState::time_wait;
      else
        value.state = SocketState::close_wait;
    }
    return {SocketResult::success, accepted};
  }

  [[nodiscard]] ReadResult read(SocketHandle handle, u8* output, u32 size) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return {SocketResult::bad_handle, 0};
    }
    if (value->read_closed) return {SocketResult::end_of_file, 0};
    if (value->receive_buffer.empty()) {
      if (value->receive_closed) {
        return {SocketResult::end_of_file, 0};
      }
      if (value->state == SocketState::reset) {
        return {value->failure, 0};
      }
      if (!is_connection(value->state) ||
          value->state == SocketState::syn_received) {
        return {SocketResult::not_connected, 0};
      }
      return {SocketResult::would_block, 0};
    }
    const auto count =
        static_cast<u32>(std::min<usize>(size, value->receive_buffer.size()));
    std::copy_n(value->receive_buffer.begin(), count, output);
    value->receive_buffer.erase(value->receive_buffer.begin(),
                                value->receive_buffer.begin() + count);
    static_cast<void>(drain_reassembly(handle, *value));
    return {SocketResult::success, count};
  }

  void reset(SocketHandle handle, SocketResult failure = SocketResult::reset) {
    if (auto* value = slot(handle); value != nullptr) {
      // An aborted handshake has no application owner. Free its backlog
      // entry instead of reporting a connection that never established to
      // accept(), which would make a serialized server fork for a dead peer.
      if (value->state == SocketState::syn_received) {
        static_cast<void>(release(handle));
        return;
      }
      value->receive_buffer.clear();
      clear_reassembly(handle);
      value->state = SocketState::reset;
      value->failure = failure;
      value->receive_closed = false;
      value->persist_waiting = false;
    }
  }

  [[nodiscard]] bool readable(SocketHandle handle) const {
    const auto* value = slot(handle);
    if (value == nullptr) {
      return false;
    }
    if (value->state == SocketState::listening) {
      return std::ranges::any_of(slots_, [&](const auto& candidate) {
        return candidate.listener == handle && !candidate.accepted &&
               (candidate.state == SocketState::established ||
                candidate.state == SocketState::close_wait ||
                candidate.state == SocketState::reset);
      });
    }
    return !value->receive_buffer.empty() || value->receive_closed ||
           value->read_closed || value->state == SocketState::reset;
  }

  [[nodiscard]] bool writable(SocketHandle handle) const {
    const auto* value = slot(handle);
    return value != nullptr &&
           (value->state == SocketState::established ||
            value->state == SocketState::close_wait) &&
           !value->send_closed;
  }

  [[nodiscard]] SocketSlot* slot(SocketHandle handle) {
    return handle.index() < slots_.size() &&
                   slots_[handle.index()].state != SocketState::free
               ? &slots_[handle.index()]
               : nullptr;
  }

  [[nodiscard]] const SocketSlot* slot(SocketHandle handle) const {
    return handle.index() < slots_.size() &&
                   slots_[handle.index()].state != SocketState::free
               ? &slots_[handle.index()]
               : nullptr;
  }

  [[nodiscard]] SocketHandle handle_of(const SocketSlot& value) const {
    return SocketHandle{static_cast<std::size_t>(&value - slots_.data())};
  }

  [[nodiscard]] std::span<SocketSlot, socket_capacity> slots() {
    return slots_;
  }

  [[nodiscard]] std::span<const SocketSlot, socket_capacity> slots() const {
    return slots_;
  }

 private:
  [[nodiscard]] static constexpr bool unspecified(Ipv4Address address) {
    return address == Ipv4Address{};
  }

  [[nodiscard]] SocketHandle allocate() const {
    const auto free =
        std::ranges::find(slots_, SocketState::free, &SocketSlot::state);
    if (free == slots_.end()) {
      return invalid_socket;
    }
    return SocketHandle{static_cast<std::size_t>(free - slots_.begin())};
  }

  [[nodiscard]] bool port_conflicts(SocketHandle excluded,
                                    Endpoint local) const {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      const SocketHandle handle{index};
      if (handle == excluded) {
        continue;
      }
      const auto& value = slots_[handle.index()];
      if (value.type == abi::socket::Type::stream &&
          value.state != SocketState::free &&
          value.state != SocketState::control &&
          value.local.port == local.port &&
          (unspecified(value.local.address) || unspecified(local.address) ||
           value.local.address == local.address)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] u16 allocate_ephemeral_port(Ipv4Address address) const {
    for (u32 port = 49152; port <= 65535; ++port) {
      if (!port_conflicts(invalid_socket,
                          Endpoint{address, static_cast<u16>(port)})) {
        return static_cast<u16>(port);
      }
    }
    return 0;
  }

  [[nodiscard]] u32 pending(SocketHandle listener_handle) const {
    return static_cast<u32>(
        std::ranges::count_if(slots_, [&](const auto& candidate) {
          return candidate.listener == listener_handle && !candidate.accepted;
        }));
  }

  [[nodiscard]] SocketResult store_reassembly(SocketHandle handle, u32 sequence,
                                              const u8* data, u32 size,
                                              bool finish) {
    if (size > reassembly_segment_capacity) {
      return SocketResult::no_space;
    }
    const ReassemblyKey key{handle, sequence};
    if (std::ranges::any_of(reassembly_, [&](const auto& slot) {
          return slot && slot->first == key;
        })) {
      return SocketResult::success;
    }
    const auto free = std::ranges::find_if(
        reassembly_, [](const auto& slot) { return !slot; });
    if (free == reassembly_.end()) return SocketResult::no_space;
    auto& available = free->emplace().second;
    (*free)->first = key;
    available.finish = finish;
    available.data.assign_range(std::span{data, size});
    return SocketResult::success;
  }

  [[nodiscard]] ReadResult drain_reassembly(SocketHandle handle,
                                            SocketSlot& value) {
    u32 total = 0;
    for (;;) {
      if (value.receive_closed) {
        clear_reassembly(handle);
        break;
      }
      auto selected = reassembly_.end();
      for (auto it = reassembly_.begin(); it != reassembly_.end(); ++it) {
        if (!*it) continue;
        const auto key = (*it)->first;
        if (key.handle != handle ||
            static_cast<i32>(key.sequence - value.receive_next) > 0) {
          continue;
        }
        // Prefer the closest segment at or before receive_next. Older fully
        // covered entries are removed on subsequent iterations.
        if (selected == reassembly_.end() ||
            static_cast<i32>(key.sequence - (*selected)->first.sequence) > 0) {
          selected = it;
        }
      }
      if (selected == reassembly_.end()) {
        break;
      }
      auto* queued = &(*selected)->second;
      const u32 acknowledged = value.receive_next - (*selected)->first.sequence;
      if (acknowledged > queued->data.size() ||
          (acknowledged == queued->data.size() && !queued->finish)) {
        selected->reset();
        continue;
      }
      const u32 remaining = queued->data.size() - acknowledged;
      if (remaining >
          value.receive_buffer.capacity() - value.receive_buffer.size()) {
        break;
      }
      const auto appended = append_received(
          value, queued->data.data() + acknowledged, remaining, queued->finish);
      total += appended.size;
      selected->reset();
      if (appended.result != SocketResult::success ||
          appended.size != remaining) {
        return {appended.result, total};
      }
    }
    return {SocketResult::success, total};
  }

  void clear_reassembly(SocketHandle handle) {
    for (auto& slot : reassembly_)
      if (slot && slot->first.handle == handle) slot.reset();
  }

  std::array<SocketSlot, socket_capacity> slots_{};
  // Fixed slots preserve packet addresses and avoid allocation or moving
  // payloads when ACKs/close remove another entry. Capacity is only sixteen.
  std::array<std::optional<std::pair<ReassemblyKey, ReassemblySegment>>,
             reassembly_capacity>
      reassembly_{};
};

}  // namespace mikos::network
