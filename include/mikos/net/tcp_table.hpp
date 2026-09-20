#pragma once

#include <charconv>
#include <mikos/net/socket.hpp>
#include <string_view>

namespace mikos::network {

inline constexpr std::string_view tcp_table_header =
    "  sl  local_address rem_address   st tx_queue rx_queue tr "
    "tm->when retrnsmt   uid  timeout inode\n";

// Each row fits in 128 bytes, including ten-digit row/inode numbers.
// Reserve the header and terminator too; a full socket table cannot truncate.
using TcpTableText =
    inplace_vector<char, tcp_table_header.size() + socket_capacity * 128 + 1>;

namespace detail {

inline void append_number(TcpTableText& text, u32 value, int base = 10,
                          usize width = 0) {
  std::array<char, 10> digits{};  // Enough for every decimal or hex u32.
  const auto result =
      std::to_chars(digits.data(), digits.data() + digits.size(), value, base);
  const std::span converted{digits.data(), result.ptr};
  if (converted.size() < width) {
    text.insert(text.end(), width - converted.size(), '0');
  }
  std::ranges::transform(converted, std::back_inserter(text), [](char digit) {
    return digit >= 'a' && digit <= 'f' ? static_cast<char>(digit - 'a' + 'A')
                                        : digit;
  });
}

inline void append_endpoint(TcpTableText& text, Endpoint endpoint) {
  // /proc/net/tcp displays addresses as little-endian hexadecimal words.
  const auto& octets = endpoint.address.octet;
  const u32 address =
      static_cast<u32>(octets[0]) | (static_cast<u32>(octets[1]) << 8) |
      (static_cast<u32>(octets[2]) << 16) | (static_cast<u32>(octets[3]) << 24);
  append_number(text, address, 16, 8);
  text.push_back(':');
  append_number(text, endpoint.port, 16, 4);
}

[[nodiscard]] constexpr std::optional<u8> proc_tcp_state(SocketState state) {
  switch (state) {
    case SocketState::listening:
      return 0x0a;
    case SocketState::syn_received:
      return 0x03;
    case SocketState::established:
      return 0x01;
    case SocketState::close_wait:
      return 0x08;
    case SocketState::fin_wait_1:
      return 0x04;
    case SocketState::fin_wait_2:
      return 0x05;
    case SocketState::time_wait:
      return 0x06;
    case SocketState::closing:
      return 0x0b;
    case SocketState::last_ack:
      return 0x09;
    default:
      return std::nullopt;
  }
}

}  // namespace detail

inline void format_tcp_table(
    std::span<const SocketSlot, socket_capacity> sockets, TcpTableText& text) {
  using namespace std::literals;
  text.assign_range(tcp_table_header);
  u32 row = 0;
  u32 inode = 0;
  for (const auto& socket : sockets) {
    ++inode;
    const auto state = detail::proc_tcp_state(socket.state);
    if (socket.type != abi::socket::Type::stream || !state) {
      continue;
    }
    text.append_range("   "sv);
    detail::append_number(text, row++);
    text.append_range(": "sv);
    detail::append_endpoint(text, socket.local);
    text.push_back(' ');
    detail::append_endpoint(text, socket.remote);
    text.push_back(' ');
    detail::append_number(text, *state, 16, 2);
    text.append_range(
        " 00000000:00000000 00:00000000 00000000   0        0 "sv);
    detail::append_number(text, inode);
    text.push_back('\n');
  }
  text.push_back('\0');
}

}  // namespace mikos::network
