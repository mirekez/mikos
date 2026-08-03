#pragma once

#include <mikos/abi/socket.hpp>
#include <mikos/container/fixed_unordered_map.hpp>
#include <mikos/net/ethernet.hpp>

namespace mikos::network {

inline constexpr u8 invalid_socket = 0xff;
inline constexpr u32 socket_capacity = 13;
inline constexpr u32 socket_receive_capacity = 4096;
inline constexpr u32 listen_backlog_capacity = 4;
inline constexpr u32 reassembly_capacity = 16;
inline constexpr u32 reassembly_segment_capacity = 1500;

enum class SocketState : u8 {
  free,
  control,
  created,
  bound,
  listening,
  syn_received,
  established,
  close_wait,
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
};

struct Endpoint {
  Ipv4Address address{};
  u16 port{};

  [[nodiscard]] constexpr bool operator==(const Endpoint&) const = default;
};

struct SocketSlot {
  SocketState state{SocketState::free};
  abi::socket::Type type{abi::socket::Type::datagram};
  Endpoint local{};
  Endpoint remote{};
  MacAddress remote_mac{};
  u32 send_next{};
  u32 receive_next{};
  u32 receive_size{};
  u16 references{};
  u8 listener{invalid_socket};
  u8 backlog{};
  bool accepted{};
  bool send_closed{};
  u8 receive_buffer[socket_receive_capacity]{};
};

struct OpenResult {
  SocketResult result{SocketResult::no_space};
  u8 handle{invalid_socket};
};

struct AcceptResult {
  SocketResult result{SocketResult::would_block};
  u8 handle{invalid_socket};
  Endpoint peer{};
};

struct ReadResult {
  SocketResult result{SocketResult::would_block};
  u32 size{};
};

struct ReassemblyKey {
  u8 handle{invalid_socket};
  u32 sequence{};

  [[nodiscard]] constexpr bool operator==(const ReassemblyKey&) const =
      default;
};

struct ReassemblyHash {
  [[nodiscard]] constexpr u32 operator()(ReassemblyKey key) const {
    return key.sequence * 2654435761u + key.handle * 97u;
  }
};

struct ReassemblySegment {
  u16 size{};
  bool finish{};
  u8 data[reassembly_segment_capacity]{};
};

class SocketTable {
 public:
  [[nodiscard]] OpenResult open(abi::socket::Type type) {
    const u8 handle = allocate();
    if (handle == invalid_socket) {
      return {};
    }
    auto& value = slots_[handle];
    value.type = type;
    value.state = type == abi::socket::Type::datagram
                      ? SocketState::control
                      : SocketState::created;
    value.references = 1;
    return {SocketResult::success, handle};
  }

  [[nodiscard]] SocketResult retain(u8 handle) {
    auto* value = slot(handle);
    if (value == nullptr || value->references == 0xffff) {
      return SocketResult::bad_handle;
    }
    ++value->references;
    return SocketResult::success;
  }

  [[nodiscard]] SocketResult release(u8 handle) {
    auto* value = slot(handle);
    if (value == nullptr || value->references == 0) {
      return SocketResult::bad_handle;
    }
    if (--value->references != 0) {
      return SocketResult::success;
    }
    if (value->state == SocketState::listening) {
      for (u8 candidate_handle = 0; candidate_handle < socket_capacity;
           ++candidate_handle) {
        auto& candidate = slots_[candidate_handle];
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

  [[nodiscard]] SocketResult bind(u8 handle, Endpoint local) {
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

  [[nodiscard]] SocketResult listen(u8 handle, u32 backlog) {
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
    value->backlog = static_cast<u8>(
        backlog > listen_backlog_capacity ? listen_backlog_capacity
                                          : (backlog == 0 ? 1 : backlog));
    value->state = SocketState::listening;
    return SocketResult::success;
  }

  [[nodiscard]] u8 listener(Endpoint local) const {
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      const auto& value = slots_[handle];
      if (value.state == SocketState::listening &&
          value.local.port == local.port &&
          (unspecified(value.local.address) ||
           value.local.address == local.address)) {
        return handle;
      }
    }
    return invalid_socket;
  }

  [[nodiscard]] u8 connection(Endpoint local, Endpoint remote) const {
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      const auto& value = slots_[handle];
      if ((value.state == SocketState::syn_received ||
           value.state == SocketState::established ||
           value.state == SocketState::close_wait ||
           value.state == SocketState::reset) &&
          value.local == local && value.remote == remote) {
        return handle;
      }
    }
    return invalid_socket;
  }

  [[nodiscard]] OpenResult begin_connection(u8 listener_handle,
                                             Endpoint local,
                                             Endpoint remote,
                                             MacAddress remote_mac,
                                             u32 remote_sequence,
                                             u32 local_sequence) {
    auto* listening = slot(listener_handle);
    if (listening == nullptr ||
        listening->state != SocketState::listening) {
      return {SocketResult::not_listening, invalid_socket};
    }
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      const auto& candidate = slots_[handle];
      if (candidate.listener == listener_handle &&
          candidate.remote == remote && !candidate.accepted) {
        return {SocketResult::success, handle};
      }
    }
    if (pending(listener_handle) >= listening->backlog) {
      return {SocketResult::no_space, invalid_socket};
    }
    const u8 handle = allocate();
    if (handle == invalid_socket) {
      return {};
    }
    auto& value = slots_[handle];
    value.state = SocketState::syn_received;
    value.type = abi::socket::Type::stream;
    value.local = local;
    value.remote = remote;
    value.remote_mac = remote_mac;
    value.send_next = local_sequence + 1;
    value.receive_next = remote_sequence + 1;
    value.references = 1;
    value.listener = listener_handle;
    return {SocketResult::success, handle};
  }

  [[nodiscard]] SocketResult establish(u8 handle, u32 acknowledgement) {
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
    return SocketResult::success;
  }

  [[nodiscard]] AcceptResult accept(u8 listener_handle) {
    const auto* listening = slot(listener_handle);
    if (listening == nullptr) {
      return {SocketResult::bad_handle, invalid_socket, {}};
    }
    if (listening->state != SocketState::listening) {
      return {SocketResult::not_listening, invalid_socket, {}};
    }
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      auto& candidate = slots_[handle];
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

  [[nodiscard]] ReadResult receive(u8 handle, u32 sequence, const u8* data,
                                   u32 size, bool finish) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return {SocketResult::bad_handle, 0};
    }
    if (value->state != SocketState::established &&
        value->state != SocketState::close_wait) {
      return {value->state == SocketState::reset ? SocketResult::reset
                                                 : SocketResult::not_connected,
              0};
    }
    const i32 distance = static_cast<i32>(sequence - value->receive_next);
    if (distance < 0) {
      // A retransmission of data already acknowledged only needs another ACK.
      return {SocketResult::success, 0};
    }
    if (distance > 0) {
      return {store_reassembly(handle, sequence, data, size, finish), 0};
    }
    u32 total = 0;
    const auto immediate = append_received(*value, data, size, finish);
    total += immediate.size;
    if (immediate.result != SocketResult::success || immediate.size != size) {
      return {immediate.result, total};
    }
    for (;;) {
      ReassemblySegment* queued =
          reassembly_.find({handle, value->receive_next});
      if (queued == nullptr ||
          queued->size > socket_receive_capacity - value->receive_size) {
        break;
      }
      const u32 queued_sequence = value->receive_next;
      const auto drained =
          append_received(*value, queued->data, queued->size, queued->finish);
      total += drained.size;
      const u32 queued_size = queued->size;
      static_cast<void>(reassembly_.erase({handle, queued_sequence}));
      if (drained.result != SocketResult::success ||
          drained.size != queued_size) {
        break;
      }
    }
    return {SocketResult::success, total};
  }

  [[nodiscard]] ReadResult append_received(SocketSlot& value, const u8* data,
                                           u32 size, bool finish) {
    u32 accepted = size;
    const u32 available = socket_receive_capacity - value.receive_size;
    if (accepted > available) {
      accepted = available;
    }
    for (u32 i = 0; i < accepted; ++i) {
      value.receive_buffer[value.receive_size + i] = data[i];
    }
    value.receive_size += accepted;
    value.receive_next += accepted;
    if (finish && accepted == size) {
      ++value.receive_next;
      value.state = SocketState::close_wait;
    }
    return {SocketResult::success, accepted};
  }

  [[nodiscard]] ReadResult read(u8 handle, u8* output, u32 size) {
    auto* value = slot(handle);
    if (value == nullptr) {
      return {SocketResult::bad_handle, 0};
    }
    if (value->receive_size == 0) {
      if (value->state == SocketState::close_wait) {
        return {SocketResult::end_of_file, 0};
      }
      if (value->state == SocketState::reset) {
        return {SocketResult::reset, 0};
      }
      if (value->state != SocketState::established) {
        return {SocketResult::not_connected, 0};
      }
      return {SocketResult::would_block, 0};
    }
    u32 count = size < value->receive_size ? size : value->receive_size;
    for (u32 i = 0; i < count; ++i) {
      output[i] = value->receive_buffer[i];
    }
    for (u32 i = count; i < value->receive_size; ++i) {
      value->receive_buffer[i - count] = value->receive_buffer[i];
    }
    value->receive_size -= count;
    return {SocketResult::success, count};
  }

  void reset(u8 handle) {
    if (auto* value = slot(handle); value != nullptr) {
      value->receive_size = 0;
      clear_reassembly(handle);
      value->state = SocketState::reset;
    }
  }

  [[nodiscard]] bool readable(u8 handle) const {
    const auto* value = slot(handle);
    if (value == nullptr) {
      return false;
    }
    if (value->state == SocketState::listening) {
      for (const auto& candidate : slots_) {
        if (candidate.listener == handle && !candidate.accepted &&
            (candidate.state == SocketState::established ||
             candidate.state == SocketState::close_wait ||
             candidate.state == SocketState::reset)) {
          return true;
        }
      }
      return false;
    }
    return value->receive_size != 0 ||
           value->state == SocketState::close_wait ||
           value->state == SocketState::reset;
  }

  [[nodiscard]] bool writable(u8 handle) const {
    const auto* value = slot(handle);
    return value != nullptr &&
           (value->state == SocketState::established ||
            value->state == SocketState::close_wait) &&
           !value->send_closed;
  }

  [[nodiscard]] SocketSlot* slot(u8 handle) {
    return handle < socket_capacity &&
                   slots_[handle].state != SocketState::free
               ? &slots_[handle]
               : nullptr;
  }

  [[nodiscard]] const SocketSlot* slot(u8 handle) const {
    return handle < socket_capacity &&
                   slots_[handle].state != SocketState::free
               ? &slots_[handle]
               : nullptr;
  }

 private:
  [[nodiscard]] static constexpr bool unspecified(Ipv4Address address) {
    return address.octet[0] == 0 && address.octet[1] == 0 &&
           address.octet[2] == 0 && address.octet[3] == 0;
  }

  [[nodiscard]] u8 allocate() const {
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      if (slots_[handle].state == SocketState::free) {
        return handle;
      }
    }
    return invalid_socket;
  }

  [[nodiscard]] bool port_conflicts(u8 excluded, Endpoint local) const {
    for (u8 handle = 0; handle < socket_capacity; ++handle) {
      if (handle == excluded) {
        continue;
      }
      const auto& value = slots_[handle];
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

  [[nodiscard]] u32 pending(u8 listener_handle) const {
    u32 count = 0;
    for (const auto& candidate : slots_) {
      if (candidate.listener == listener_handle && !candidate.accepted) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] SocketResult store_reassembly(u8 handle, u32 sequence,
                                               const u8* data, u32 size,
                                               bool finish) {
    if (size > reassembly_segment_capacity) {
      return SocketResult::no_space;
    }
    bool inserted = false;
    auto* available =
        reassembly_.find_or_emplace({handle, sequence}, inserted);
    if (available == nullptr) {
      return SocketResult::no_space;
    }
    if (!inserted) {
      return SocketResult::success;
    }
    available->size = static_cast<u16>(size);
    available->finish = finish;
    for (u32 i = 0; i < size; ++i) {
      available->data[i] = data[i];
    }
    return SocketResult::success;
  }

  void clear_reassembly(u8 handle) {
    reassembly_.erase_if(
        [handle](ReassemblyKey key) { return key.handle == handle; });
  }

  SocketSlot slots_[socket_capacity]{};
  container::FixedUnorderedMap<ReassemblyKey, ReassemblySegment,
                               reassembly_capacity, ReassemblyHash>
      reassembly_{};
};

}  // namespace mikos::network
