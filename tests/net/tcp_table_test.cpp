#include <mikos/net/tcp_table.hpp>
#include <support/test.hpp>

int main() {
  using namespace mikos;
  using namespace mikos::network;
  test::Suite suite{"net/tcp_table"};
  SocketTable sockets;
  TcpTableText text;

  format_tcp_table(sockets.slots(), text);
  MIKOS_CHECK(suite, std::string_view(text.data()) == tcp_table_header);

  // Preserve Linux's byte order, uppercase hex, zero padding, state codes,
  // consecutive display rows, and handle-based inodes across skipped slots.
  const std::array states{SocketState::listening, SocketState::syn_received,
                          SocketState::established, SocketState::close_wait};
  auto slots = sockets.slots();
  for (usize i = 0; i < states.size(); ++i) {
    auto& socket = slots[i + 1];
    socket.type = abi::socket::Type::stream;
    socket.state = states[i];
    socket.local = {{{192, 168, 76, 2}}, 22};
    socket.remote = {{{10, 0, 2, 15}}, 0xabcd};
  }
  // Neither a datagram socket nor a reset TCP socket belongs in the output.
  slots[0].state = SocketState::control;
  slots[5].type = abi::socket::Type::stream;
  slots[5].state = SocketState::reset;
  format_tcp_table(slots, text);
  const auto rows =
      std::string_view(text.data()).substr(tcp_table_header.size());
  MIKOS_CHECK(suite,
              rows ==
                  "   0: 024CA8C0:0016 0F02000A:ABCD 0A 00000000:00000000 "
                  "00:00000000 00000000   0        0 2\n"
                  "   1: 024CA8C0:0016 0F02000A:ABCD 03 00000000:00000000 "
                  "00:00000000 00000000   0        0 3\n"
                  "   2: 024CA8C0:0016 0F02000A:ABCD 01 00000000:00000000 "
                  "00:00000000 00000000   0        0 4\n"
                  "   3: 024CA8C0:0016 0F02000A:ABCD 08 00000000:00000000 "
                  "00:00000000 00000000   0        0 5\n");

  for (auto& socket : slots) {
    socket.type = abi::socket::Type::stream;
    socket.state = SocketState::listening;
    socket.local = {{{255, 255, 255, 255}}, 65535};
    socket.remote = {};
  }
  format_tcp_table(slots, text);
  const std::string_view full{text.data()};
  MIKOS_CHECK(suite, text.back() == '\0');
  MIKOS_CHECK(suite, text.size() < text.capacity());
  MIKOS_CHECK(suite, std::ranges::count(full, '\n') == socket_capacity + 1);
  MIKOS_CHECK(
      suite,
      full.ends_with("   12: FFFFFFFF:FFFF 00000000:0000 0A 00000000:00000000 "
                     "00:00000000 00000000   0        0 13\n"));

  // Reusing the buffer after all sockets close must discard every old row.
  std::ranges::fill(slots, SocketSlot{});
  format_tcp_table(slots, text);
  MIKOS_CHECK(suite, std::string_view(text.data()) == tcp_table_header);
  MIKOS_CHECK(suite, text.size() == tcp_table_header.size() + 1);
  return suite.finish();
}
