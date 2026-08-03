#include <mikos/net/socket.hpp>

#include <support/test.hpp>

namespace {

constexpr mikos::Ipv4Address any{{0, 0, 0, 0}};
constexpr mikos::Ipv4Address local_ip{{192, 168, 76, 2}};
constexpr mikos::Ipv4Address peer_ip{{192, 168, 76, 1}};
constexpr mikos::MacAddress peer_mac{{2, 0, 0, 0, 0, 1}};

}  // namespace

int main() {
  mikos::test::Suite suite{"net/socket"};
  using mikos::abi::socket::Type;
  using mikos::network::Endpoint;
  using mikos::network::SocketResult;
  using mikos::network::SocketState;
  mikos::network::SocketTable sockets;

  const auto control = sockets.open(Type::datagram);
  MIKOS_CHECK(suite, control.result == SocketResult::success);
  MIKOS_CHECK(suite, sockets.slot(control.handle)->state ==
                         SocketState::control);
  MIKOS_CHECK(suite, sockets.bind(control.handle, {any, 22}) ==
                         SocketResult::wrong_type);

  const auto listener = sockets.open(Type::stream);
  MIKOS_CHECK(suite, listener.result == SocketResult::success);
  MIKOS_CHECK(suite, sockets.listen(listener.handle, 4) ==
                         SocketResult::not_bound);
  MIKOS_CHECK(suite, sockets.bind(listener.handle, {any, 22}) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, sockets.listen(listener.handle, 99) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, sockets.slot(listener.handle)->backlog == 4);
  MIKOS_CHECK(suite, sockets.listener({local_ip, 22}) == listener.handle);
  MIKOS_CHECK(suite, !sockets.readable(listener.handle));

  const auto duplicate = sockets.open(Type::stream);
  MIKOS_CHECK(suite, sockets.bind(duplicate.handle, {local_ip, 22}) ==
                         SocketResult::address_in_use);
  MIKOS_CHECK(suite, sockets.release(duplicate.handle) ==
                         SocketResult::success);

  const auto pending = sockets.begin_connection(
      listener.handle, Endpoint{local_ip, 22}, Endpoint{peer_ip, 49152},
      peer_mac, 1000, 5000);
  MIKOS_CHECK(suite, pending.result == SocketResult::success);
  MIKOS_CHECK(suite, sockets.slot(pending.handle)->receive_next == 1001);
  MIKOS_CHECK(suite, sockets.slot(pending.handle)->send_next == 5001);
  const auto retransmit = sockets.begin_connection(
      listener.handle, Endpoint{local_ip, 22}, Endpoint{peer_ip, 49152},
      peer_mac, 1000, 9000);
  MIKOS_CHECK(suite, retransmit.handle == pending.handle);
  MIKOS_CHECK(suite, sockets.establish(pending.handle, 5000) ==
                         SocketResult::invalid_argument);
  MIKOS_CHECK(suite, sockets.establish(pending.handle, 5001) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, sockets.readable(listener.handle));

  const auto accepted = sockets.accept(listener.handle);
  MIKOS_CHECK(suite, accepted.result == SocketResult::success);
  MIKOS_CHECK(suite, accepted.handle == pending.handle);
  MIKOS_CHECK(suite, (accepted.peer == Endpoint{peer_ip, 49152}));
  MIKOS_CHECK(suite, sockets.accept(listener.handle).result ==
                         SocketResult::would_block);

  const mikos::u8 hello[]{'h', 'e', 'l', 'l', 'o'};
  MIKOS_CHECK(suite,
              sockets.receive(accepted.handle, 999, hello, sizeof(hello), false)
                      .result == SocketResult::success);
  const auto received = sockets.receive(accepted.handle, 1001, hello,
                                        sizeof(hello), false);
  MIKOS_CHECK(suite, received.result == SocketResult::success);
  MIKOS_CHECK(suite, received.size == sizeof(hello));
  MIKOS_CHECK(suite, sockets.readable(accepted.handle));
  mikos::u8 first[2]{};
  const auto first_read = sockets.read(accepted.handle, first, sizeof(first));
  MIKOS_CHECK(suite, first_read.size == 2);
  MIKOS_CHECK(suite, first[0] == 'h' && first[1] == 'e');
  mikos::u8 rest[8]{};
  const auto rest_read = sockets.read(accepted.handle, rest, sizeof(rest));
  MIKOS_CHECK(suite, rest_read.size == 3);
  MIKOS_CHECK(suite, rest[0] == 'l' && rest[2] == 'o');
  MIKOS_CHECK(suite, sockets.read(accepted.handle, rest, sizeof(rest)).result ==
                         SocketResult::would_block);

  MIKOS_CHECK(suite,
              sockets.receive(accepted.handle, 1006, nullptr, 0, true).result ==
                  SocketResult::success);
  MIKOS_CHECK(suite, sockets.read(accepted.handle, rest, sizeof(rest)).result ==
                         SocketResult::end_of_file);
  MIKOS_CHECK(suite, sockets.writable(accepted.handle));

  MIKOS_CHECK(suite, sockets.retain(accepted.handle) == SocketResult::success);
  MIKOS_CHECK(suite, sockets.release(accepted.handle) == SocketResult::success);
  MIKOS_CHECK(suite, sockets.slot(accepted.handle) != nullptr);
  MIKOS_CHECK(suite, sockets.release(accepted.handle) == SocketResult::success);
  MIKOS_CHECK(suite, sockets.slot(accepted.handle) == nullptr);
  MIKOS_CHECK(suite, sockets.release(listener.handle) == SocketResult::success);
  MIKOS_CHECK(suite, sockets.release(control.handle) == SocketResult::success);

  mikos::network::SocketTable reordered;
  const auto reordered_listener = reordered.open(Type::stream);
  MIKOS_CHECK(suite,
              reordered.bind(reordered_listener.handle, {any, 24}) ==
                  SocketResult::success);
  MIKOS_CHECK(suite, reordered.listen(reordered_listener.handle, 1) ==
                         SocketResult::success);
  const auto reordered_child = reordered.begin_connection(
      reordered_listener.handle, {local_ip, 24}, {peer_ip, 42000}, peer_mac,
      100, 200);
  MIKOS_CHECK(suite, reordered.establish(reordered_child.handle, 201) ==
                         SocketResult::success);
  const mikos::u8 tail[]{'l', 'o'};
  MIKOS_CHECK(
      suite,
      reordered.receive(reordered_child.handle, 104, tail, sizeof(tail), true)
              .result == SocketResult::success);
  MIKOS_CHECK(suite, !reordered.readable(reordered_child.handle));
  const mikos::u8 head[]{'h', 'e', 'l'};
  const auto assembled = reordered.receive(reordered_child.handle, 101, head,
                                           sizeof(head), false);
  MIKOS_CHECK(suite, assembled.result == SocketResult::success);
  MIKOS_CHECK(suite, assembled.size == 5);
  mikos::u8 assembled_output[8]{};
  MIKOS_CHECK(suite,
              reordered
                      .read(reordered_child.handle, assembled_output,
                            sizeof(assembled_output))
                      .size == 5);
  MIKOS_CHECK(suite, assembled_output[0] == 'h' &&
                         assembled_output[4] == 'o');
  MIKOS_CHECK(suite,
              reordered
                      .read(reordered_child.handle, assembled_output,
                            sizeof(assembled_output))
                      .result == SocketResult::end_of_file);
  MIKOS_CHECK(suite, reordered.writable(reordered_child.handle));

  mikos::network::SocketTable conflicts;
  const auto exact_one = conflicts.open(Type::stream);
  const auto exact_two = conflicts.open(Type::stream);
  MIKOS_CHECK(suite, conflicts.bind(exact_one.handle, {local_ip, 80}) ==
                         SocketResult::success);
  MIKOS_CHECK(
      suite,
      conflicts.bind(exact_two.handle,
                     {mikos::Ipv4Address{{192, 168, 76, 3}}, 80}) ==
          SocketResult::success);
  const auto wildcard = conflicts.open(Type::stream);
  MIKOS_CHECK(suite, conflicts.bind(wildcard.handle, {any, 80}) ==
                         SocketResult::address_in_use);
  const auto ephemeral_one = conflicts.open(Type::stream);
  const auto ephemeral_two = conflicts.open(Type::stream);
  MIKOS_CHECK(suite, conflicts.bind(ephemeral_one.handle, {any, 0}) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, conflicts.bind(ephemeral_two.handle, {any, 0}) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, conflicts.slot(ephemeral_one.handle)->local.port !=
                         conflicts.slot(ephemeral_two.handle)->local.port);

  mikos::network::SocketTable backlog;
  const auto backlog_listener = backlog.open(Type::stream);
  MIKOS_CHECK(suite, backlog.bind(backlog_listener.handle, {any, 22}) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, backlog.listen(backlog_listener.handle, 1) ==
                         SocketResult::success);
  const auto first_pending = backlog.begin_connection(
      backlog_listener.handle, {local_ip, 22}, {peer_ip, 40000}, peer_mac, 1,
      10);
  MIKOS_CHECK(suite, first_pending.result == SocketResult::success);
  MIKOS_CHECK(suite,
              backlog
                      .begin_connection(backlog_listener.handle,
                                        {local_ip, 22}, {peer_ip, 40001},
                                        peer_mac, 1, 20)
                      .result == SocketResult::no_space);
  MIKOS_CHECK(suite, backlog.release(backlog_listener.handle) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, backlog.slot(first_pending.handle) == nullptr);

  mikos::network::SocketTable reset_table;
  const auto reset_listener = reset_table.open(Type::stream);
  MIKOS_CHECK(suite, reset_table.bind(reset_listener.handle, {any, 23}) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, reset_table.listen(reset_listener.handle, 1) ==
                         SocketResult::success);
  const auto reset_child = reset_table.begin_connection(
      reset_listener.handle, {local_ip, 23}, {peer_ip, 41000}, peer_mac, 1,
      20);
  MIKOS_CHECK(suite, reset_table.establish(reset_child.handle, 21) ==
                         SocketResult::success);
  reset_table.reset(reset_child.handle);
  MIKOS_CHECK(suite, reset_table.readable(reset_listener.handle));
  const auto reset_accepted = reset_table.accept(reset_listener.handle);
  MIKOS_CHECK(suite, reset_accepted.result == SocketResult::success);
  mikos::u8 reset_output{};
  MIKOS_CHECK(suite,
              reset_table.read(reset_accepted.handle, &reset_output, 1).result ==
                  SocketResult::reset);

  mikos::network::SocketTable capacity;
  for (mikos::u32 i = 0; i < mikos::network::socket_capacity; ++i) {
    MIKOS_CHECK(suite, capacity.open(Type::stream).result ==
                           SocketResult::success);
  }
  MIKOS_CHECK(suite, capacity.open(Type::stream).result ==
                         SocketResult::no_space);

  return suite.finish();
}
