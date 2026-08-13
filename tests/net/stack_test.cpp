#include <drivers/net/net.hpp>
#include <mikos/kernel.hpp>
#include <mikos/net/tcp.hpp>

#include <support/test.hpp>

namespace {

constexpr mikos::MacAddress guest_mac{{2, 0, 0, 0, 0, 2}};
constexpr mikos::MacAddress peer_mac{{2, 0, 0, 0, 0, 1}};
constexpr mikos::Ipv4Address guest_ip{{10, 0, 2, 15}};
constexpr mikos::Ipv4Address peer_ip{{10, 0, 2, 2}};

alignas(4) mikos::u8 receive_frame[1536]{};
mikos::u32 receive_size{};
bool receive_ready{};
alignas(4) mikos::u8 transmitted_frame[1536]{};
mikos::u32 transmitted_size{};
mikos::u32 transmit_count{};
mikos::u32 transmit_failures{};

void make_packet(mikos::u8 flags, mikos::u32 sequence,
                 mikos::u32 acknowledgement,
                 const mikos::u8* payload = nullptr,
                 mikos::u32 payload_size = 0) {
  using namespace mikos;
  const u32 headers =
      sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(TcpHeader);
  for (u32 i = 0; i < headers + payload_size; ++i) {
    receive_frame[i] = 0;
  }
  auto& ethernet = *reinterpret_cast<EthernetHeader*>(receive_frame);
  auto& ip = *reinterpret_cast<Ipv4Header*>(receive_frame + sizeof(ethernet));
  auto& tcp = *reinterpret_cast<TcpHeader*>(
      receive_frame + sizeof(ethernet) + sizeof(ip));
  copy_octets(ethernet.source, peer_mac.octet);
  copy_octets(ethernet.destination, guest_mac.octet);
  ethernet.type = net16(0x0800);
  ip.version_ihl = 0x45;
  ip.total_length =
      net16(static_cast<u16>(sizeof(ip) + sizeof(tcp) + payload_size));
  ip.ttl = 64;
  ip.protocol = 6;
  copy_octets(ip.source, peer_ip.octet);
  copy_octets(ip.destination, guest_ip.octet);
  tcp.source_port = net16(49152);
  tcp.destination_port = net16(22);
  tcp.sequence = net32(sequence);
  tcp.acknowledgement = net32(acknowledgement);
  tcp.data_offset = 5 << 4;
  tcp.flags = flags;
  tcp.window = net16(8192);
  for (u32 i = 0; i < payload_size; ++i) {
    receive_frame[headers + i] = payload[i];
  }
  tcp.checksum = net16(tcp_checksum(
      peer_ip, guest_ip, reinterpret_cast<const u8*>(&tcp),
      sizeof(tcp) + payload_size));
  ip.checksum = net16(
      internet_checksum(reinterpret_cast<const u8*>(&ip), sizeof(ip)));
  receive_size = headers + payload_size;
  receive_ready = true;
}

[[nodiscard]] bool transmitted_tcp(mikos::TcpView& view) {
  return mikos::parse_tcp(transmitted_frame, transmitted_size, peer_ip, view);
}

[[nodiscard]] bool contains(const char* text, const char* needle) {
  for (mikos::u32 start = 0; text[start] != '\0'; ++start) {
    mikos::u32 index = 0;
    while (needle[index] != '\0' && text[start + index] == needle[index]) {
      ++index;
    }
    if (needle[index] == '\0') {
      return true;
    }
  }
  return false;
}

}  // namespace

namespace mikos::drivers::net {

bool initialize() { return true; }

MacAddress mac_address() { return guest_mac; }

bool receive(Frame& frame) {
  if (!receive_ready) {
    return false;
  }
  frame = {receive_frame, receive_size};
  receive_ready = false;
  return true;
}

bool transmit(const u8* frame, u32 size) {
  if (transmit_failures != 0) {
    --transmit_failures;
    return false;
  }
  if (size > sizeof(transmitted_frame)) {
    return false;
  }
  for (u32 i = 0; i < size; ++i) {
    transmitted_frame[i] = frame[i];
  }
  transmitted_size = size;
  ++transmit_count;
  return true;
}

}  // namespace mikos::drivers::net

namespace mikos {

void uart_put(char) {}
void uart_write(const char*, u32) {}
void write_text(const char*) {}
void write_u32(u32) {}

}  // namespace mikos

int main() {
  using namespace mikos;
  using namespace mikos::network;
  test::Suite suite{"net/stack"};

  MIKOS_CHECK(suite, initialize());
  const auto listener = socket_open(abi::socket::Type::stream);
  MIKOS_CHECK(suite, listener.result == SocketResult::success);
  MIKOS_CHECK(suite,
              socket_bind(listener.handle, Endpoint{{{0, 0, 0, 0}}, 22}) ==
                  SocketResult::success);
  MIKOS_CHECK(suite, socket_listen(listener.handle, 2) ==
                         SocketResult::success);
  MIKOS_CHECK(suite, contains(tcp_table(), "00000000:0016"));
  MIKOS_CHECK(suite, contains(tcp_table(), " 0A "));

  make_packet(tcp_syn, 1000, 0);
  poll();
  MIKOS_CHECK(suite, transmit_count == 1);
  TcpView syn_ack{};
  MIKOS_CHECK(suite, transmitted_tcp(syn_ack));
  MIKOS_CHECK(suite, syn_ack.tcp->flags == (tcp_syn | tcp_ack));
  MIKOS_CHECK(suite, net32(syn_ack.tcp->acknowledgement) == 1001);
  const u32 server_sequence = net32(syn_ack.tcp->sequence);

  make_packet(tcp_ack, 1001, server_sequence + 1);
  poll();
  MIKOS_CHECK(suite, socket_readable(listener.handle));
  const auto accepted = socket_accept(listener.handle);
  MIKOS_CHECK(suite, accepted.result == SocketResult::success);
  MIKOS_CHECK(suite, accepted.peer.port == 49152);
  MIKOS_CHECK(suite, accepted.peer.address == peer_ip);

  const u8 request[]{'p', 'i', 'n', 'g'};
  make_packet(tcp_ack | tcp_psh, 1001, server_sequence + 1, request,
              sizeof(request));
  poll();
  MIKOS_CHECK(suite, transmit_count == 2);
  TcpView data_ack{};
  MIKOS_CHECK(suite, transmitted_tcp(data_ack));
  MIKOS_CHECK(suite, data_ack.tcp->flags == tcp_ack);
  MIKOS_CHECK(suite, net32(data_ack.tcp->acknowledgement) == 1005);
  u8 input[8]{};
  const auto read_result = socket_read(accepted.handle, input, sizeof(input));
  MIKOS_CHECK(suite, read_result.result == SocketResult::success);
  MIKOS_CHECK(suite, read_result.size == sizeof(request));
  MIKOS_CHECK(suite, input[0] == 'p' && input[3] == 'g');

  const u8 response[]{'p', 'o', 'n', 'g'};
  const auto write_result =
      socket_write(accepted.handle, response, sizeof(response));
  MIKOS_CHECK(suite, write_result.result == SocketResult::success);
  MIKOS_CHECK(suite, write_result.size == sizeof(response));
  TcpView response_view{};
  MIKOS_CHECK(suite, transmitted_tcp(response_view));
  MIKOS_CHECK(suite,
              response_view.tcp->flags == (tcp_ack | tcp_psh));
  MIKOS_CHECK(suite, net32(response_view.tcp->sequence) ==
                         server_sequence + 1);
  MIKOS_CHECK(suite, response_view.payload_size == sizeof(response));
  MIKOS_CHECK(suite, response_view.payload[1] == 'o');

  // Outbound payload is retained until a cumulative ACK covers it. This is
  // essential for SSH, which emits adjacent identification/KEX records and
  // cannot recover if either TCP segment is silently lost by the polling NIC.
  const u32 before_response_retry = transmit_count;
  for (u32 i = 0; i < 255; ++i) {
    poll();
  }
  MIKOS_CHECK(suite, transmit_count == before_response_retry);
  poll();
  MIKOS_CHECK(suite, transmit_count == before_response_retry + 1);
  TcpView response_retry{};
  MIKOS_CHECK(suite, transmitted_tcp(response_retry));
  MIKOS_CHECK(suite, net32(response_retry.tcp->sequence) ==
                         server_sequence + 1);
  MIKOS_CHECK(suite, response_retry.payload_size == sizeof(response));

  // Two writes occupy distinct sequence-keyed slots. A cumulative ACK of the
  // first must leave only the second eligible for retransmission.
  make_packet(tcp_ack, 1005, server_sequence + 5);
  poll();
  transmit_failures = 1;
  const u8 failed_write[]{'?'};
  const auto failed_write_result =
      socket_write(accepted.handle, failed_write, sizeof(failed_write));
  MIKOS_CHECK(suite,
              failed_write_result.result == SocketResult::no_space);
  const u32 after_failed_write = transmit_count;
  for (u32 i = 0; i < 300; ++i) {
    poll();
  }
  MIKOS_CHECK(suite, transmit_count == after_failed_write);
  const u8 first_followup[]{'a', 'b', 'c'};
  const u8 second_followup[]{'d', 'e'};
  MIKOS_CHECK(suite,
              socket_write(accepted.handle, first_followup,
                           sizeof(first_followup)).size ==
                  sizeof(first_followup));
  MIKOS_CHECK(suite,
              socket_write(accepted.handle, second_followup,
                           sizeof(second_followup)).size ==
                  sizeof(second_followup));
  TcpView second_followup_view{};
  MIKOS_CHECK(suite, transmitted_tcp(second_followup_view));
  MIKOS_CHECK(suite, net32(second_followup_view.tcp->sequence) ==
                         server_sequence + 8);
  make_packet(tcp_ack, 1005, server_sequence + 8);
  poll();
  const u32 before_second_retry = transmit_count;
  for (u32 i = 0; i < 254; ++i) {
    poll();
  }
  MIKOS_CHECK(suite, transmit_count == before_second_retry);
  poll();
  MIKOS_CHECK(suite, transmit_count == before_second_retry + 1);
  TcpView second_retry{};
  MIKOS_CHECK(suite, transmitted_tcp(second_retry));
  MIKOS_CHECK(suite, net32(second_retry.tcp->sequence) ==
                         server_sequence + 8);
  MIKOS_CHECK(suite, second_retry.payload_size == sizeof(second_followup));
  MIKOS_CHECK(suite, second_retry.payload[0] == 'd');
  make_packet(tcp_ack, 1005, server_sequence + 10);
  poll();

  // If the polling NIC cannot send the immediate ACK, a later poll retries it.
  // One further duplicate is retained for a silent compute interval, but no
  // third duplicate is emitted (which could trigger fast retransmit).
  const u8 slow_request[]{'x'};
  transmit_failures = 1;
  make_packet(tcp_ack | tcp_psh, 1005, server_sequence + 10, slow_request,
              sizeof(slow_request));
  const u32 before_failed_ack = transmit_count;
  poll();
  MIKOS_CHECK(suite, transmit_count == before_failed_ack);
  poll();
  MIKOS_CHECK(suite, transmit_count == before_failed_ack + 1);
  TcpView retried_ack{};
  MIKOS_CHECK(suite, transmitted_tcp(retried_ack));
  MIKOS_CHECK(suite, retried_ack.tcp->flags == tcp_ack);
  MIKOS_CHECK(suite, net32(retried_ack.tcp->acknowledgement) == 1006);
  poll();
  MIKOS_CHECK(suite, transmit_count == before_failed_ack + 2);
  poll();
  MIKOS_CHECK(suite, transmit_count == before_failed_ack + 2);
  MIKOS_CHECK(suite,
              socket_read(accepted.handle, input, sizeof(input)).size == 1);

  make_packet(tcp_ack | tcp_fin, 1006, server_sequence + 10);
  poll();
  MIKOS_CHECK(suite,
              socket_read(accepted.handle, input, sizeof(input)).result ==
                  SocketResult::end_of_file);
  MIKOS_CHECK(suite, socket_writable(accepted.handle));
  const u8 half_close_response[]{'!'};
  const auto half_close_write =
      socket_write(accepted.handle, half_close_response,
                   sizeof(half_close_response));
  MIKOS_CHECK(suite, half_close_write.result == SocketResult::success);
  MIKOS_CHECK(suite,
              half_close_write.size == sizeof(half_close_response));
  TcpView half_close_view{};
  MIKOS_CHECK(suite, transmitted_tcp(half_close_view));
  MIKOS_CHECK(suite,
              half_close_view.tcp->flags == (tcp_ack | tcp_psh));
  MIKOS_CHECK(suite, half_close_view.payload_size == 1);
  MIKOS_CHECK(suite, half_close_view.payload[0] == '!');
  MIKOS_CHECK(suite, socket_close(accepted.handle) == SocketResult::success);
  TcpView final_fin{};
  MIKOS_CHECK(suite, transmitted_tcp(final_fin));
  MIKOS_CHECK(suite, final_fin.tcp->flags == (tcp_fin | tcp_ack));
  const u32 before_closed_polling = transmit_count;
  for (u32 i = 0; i < 300; ++i) {
    poll();
  }
  MIKOS_CHECK(suite, transmit_count == before_closed_polling);
  MIKOS_CHECK(suite, socket_close(listener.handle) == SocketResult::success);

  const u32 before_corrupt = transmit_count;
  make_packet(tcp_syn, 2000, 0);
  receive_frame[receive_size - 1] ^= 1;
  poll();
  MIKOS_CHECK(suite, transmit_count == before_corrupt);

  return suite.finish();
}
