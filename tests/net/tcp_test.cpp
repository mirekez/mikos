#include <mikos/net/tcp.hpp>

#include <support/test.hpp>

namespace {

constexpr mikos::MacAddress local_mac{{2, 0, 0, 0, 0, 2}};
constexpr mikos::MacAddress peer_mac{{2, 0, 0, 0, 0, 1}};
constexpr mikos::Ipv4Address local_ip{{192, 168, 76, 2}};
constexpr mikos::Ipv4Address peer_ip{{192, 168, 76, 1}};

struct [[gnu::packed]] Packet {
  mikos::EthernetHeader ethernet;
  mikos::Ipv4Header ip;
  mikos::TcpHeader tcp;
  mikos::u8 payload[4];
};

struct [[gnu::packed]] OptionPacket {
  mikos::EthernetHeader ethernet;
  mikos::Ipv4Header ip;
  mikos::u8 ip_options[4];
  mikos::TcpHeader tcp;
  mikos::u8 tcp_options[4];
  mikos::u8 payload[4];
};

[[nodiscard]] Packet packet() {
  Packet value{};
  mikos::copy_octets(value.ethernet.source, peer_mac.octet);
  mikos::copy_octets(value.ethernet.destination, local_mac.octet);
  value.ethernet.type = mikos::net16(0x0800);
  value.ip.version_ihl = 0x45;
  value.ip.total_length =
      mikos::net16(sizeof(Packet) - sizeof(mikos::EthernetHeader));
  value.ip.identification = mikos::net16(7);
  value.ip.ttl = 64;
  value.ip.protocol = 6;
  mikos::copy_octets(value.ip.source, peer_ip.octet);
  mikos::copy_octets(value.ip.destination, local_ip.octet);
  value.tcp.source_port = mikos::net16(49152);
  value.tcp.destination_port = mikos::net16(22);
  value.tcp.sequence = mikos::net32(1000);
  value.tcp.acknowledgement = mikos::net32(5001);
  value.tcp.data_offset = 5 << 4;
  value.tcp.flags = mikos::tcp_ack | mikos::tcp_psh;
  value.tcp.window = mikos::net16(4096);
  value.payload[0] = 't';
  value.payload[1] = 'e';
  value.payload[2] = 's';
  value.payload[3] = 't';
  value.tcp.checksum = mikos::net16(mikos::tcp_checksum(
      peer_ip, local_ip, reinterpret_cast<const mikos::u8*>(&value.tcp),
      sizeof(value.tcp) + sizeof(value.payload)));
  value.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&value.ip), sizeof(value.ip)));
  return value;
}

}  // namespace

int main() {
  mikos::test::Suite suite{"net/tcp"};
  auto valid = packet();
  mikos::TcpView view{};
  MIKOS_CHECK(suite,
              mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&valid),
                               sizeof(valid), local_ip, view));
  MIKOS_CHECK(suite, view.tcp->source_port == mikos::net16(49152));
  MIKOS_CHECK(suite, view.payload_size == 4);
  MIKOS_CHECK(suite, view.payload[0] == 't');
  MIKOS_CHECK(suite, mikos::tcp_checksum(
                         peer_ip, local_ip,
                         reinterpret_cast<const mikos::u8*>(&valid.tcp),
                         sizeof(valid.tcp) + sizeof(valid.payload)) == 0);

  const auto base = packet();
  OptionPacket options{};
  options.ethernet = base.ethernet;
  options.ip = base.ip;
  options.tcp = base.tcp;
  for (mikos::u32 i = 0; i < sizeof(options.payload); ++i) {
    options.payload[i] = base.payload[i];
  }
  options.ip.version_ihl = 0x46;
  options.ip.total_length = mikos::net16(
      sizeof(options) - sizeof(mikos::EthernetHeader));
  options.ip.checksum = 0;
  options.tcp.data_offset = 6 << 4;
  options.tcp_options[0] = 1;
  options.tcp_options[1] = 1;
  options.tcp.checksum = 0;
  options.tcp.checksum = mikos::net16(mikos::tcp_checksum(
      peer_ip, local_ip, reinterpret_cast<const mikos::u8*>(&options.tcp),
      sizeof(options.tcp) + sizeof(options.tcp_options) +
          sizeof(options.payload)));
  options.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&options.ip),
      sizeof(options.ip) + sizeof(options.ip_options)));
  MIKOS_CHECK(suite,
              mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&options),
                               sizeof(options), local_ip, view));
  MIKOS_CHECK(suite, view.payload_size == sizeof(options.payload));
  MIKOS_CHECK(suite, view.payload[0] == 't' && view.payload[3] == 't');

  auto short_packet = packet();
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&short_packet),
                                sizeof(mikos::EthernetHeader) +
                                    sizeof(mikos::Ipv4Header) +
                                    sizeof(mikos::TcpHeader) - 1,
                                local_ip, view));
  auto wrong_destination = packet();
  wrong_destination.ip.destination[3] = 99;
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(
                  reinterpret_cast<mikos::u8*>(&wrong_destination),
                  sizeof(wrong_destination), local_ip, view));
  auto corrupt_ip = packet();
  corrupt_ip.ip.ttl ^= 1;
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&corrupt_ip),
                                sizeof(corrupt_ip), local_ip, view));
  auto corrupt_tcp = packet();
  corrupt_tcp.payload[0] ^= 1;
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&corrupt_tcp),
                                sizeof(corrupt_tcp), local_ip, view));
  auto fragment = packet();
  fragment.ip.flags_fragment = mikos::net16(1);
  fragment.ip.checksum = 0;
  fragment.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&fragment.ip), sizeof(fragment.ip)));
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&fragment),
                                sizeof(fragment), local_ip, view));
  auto short_ip_total = packet();
  short_ip_total.ip.total_length = mikos::net16(
      sizeof(mikos::Ipv4Header) + sizeof(mikos::TcpHeader) - 1);
  short_ip_total.ip.checksum = 0;
  short_ip_total.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&short_ip_total.ip),
      sizeof(short_ip_total.ip)));
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(
                  reinterpret_cast<mikos::u8*>(&short_ip_total),
                  sizeof(short_ip_total), local_ip, view));
  auto bad_tcp_offset = packet();
  bad_tcp_offset.tcp.data_offset = 4 << 4;
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(
                  reinterpret_cast<mikos::u8*>(&bad_tcp_offset),
                  sizeof(bad_tcp_offset), local_ip, view));
  auto wrong_protocol = packet();
  wrong_protocol.ip.protocol = 17;
  MIKOS_CHECK(suite,
              !mikos::parse_tcp(reinterpret_cast<mikos::u8*>(&wrong_protocol),
                                sizeof(wrong_protocol), local_ip, view));

  return suite.finish();
}
