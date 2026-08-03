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

  make_packet(tcp_ack | tcp_fin, 1005, server_sequence + 5);
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
  MIKOS_CHECK(suite, socket_close(listener.handle) == SocketResult::success);

  const u32 before_corrupt = transmit_count;
  make_packet(tcp_syn, 2000, 0);
  receive_frame[receive_size - 1] ^= 1;
  poll();
  MIKOS_CHECK(suite, transmit_count == before_corrupt);

  return suite.finish();
}
