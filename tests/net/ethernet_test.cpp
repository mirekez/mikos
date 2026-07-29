#include <mikos/net/ethernet.hpp>

#include <support/test.hpp>

namespace {

constexpr mikos::MacAddress guest_mac{{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
constexpr mikos::MacAddress peer_mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
constexpr mikos::Ipv4Address guest_ip{{10, 0, 2, 15}};
constexpr mikos::Ipv4Address peer_ip{{10, 0, 2, 2}};

struct [[gnu::packed]] EchoPacket {
  mikos::EthernetHeader ethernet;
  mikos::Ipv4Header ip;
  mikos::IcmpEchoHeader icmp;
  mikos::u8 payload[4];
};

[[nodiscard]] mikos::ArpIpv4 arp_request() {
  mikos::ArpIpv4 packet{};
  packet.ethernet.type = mikos::net16(0x0806);
  packet.hardware_type = mikos::net16(1);
  packet.protocol_type = mikos::net16(0x0800);
  packet.hardware_size = 6;
  packet.protocol_size = 4;
  packet.operation = mikos::net16(1);
  mikos::copy_octets(packet.sender_mac, peer_mac.octet);
  mikos::copy_octets(packet.sender_ip, peer_ip.octet);
  mikos::copy_octets(packet.target_ip, guest_ip.octet);
  return packet;
}

[[nodiscard]] EchoPacket echo_request() {
  EchoPacket packet{};
  mikos::copy_octets(packet.ethernet.source, peer_mac.octet);
  mikos::copy_octets(packet.ethernet.destination, guest_mac.octet);
  packet.ethernet.type = mikos::net16(0x0800);
  packet.ip.version_ihl = 0x45;
  packet.ip.total_length =
      mikos::net16(sizeof(packet) - sizeof(mikos::EthernetHeader));
  packet.ip.ttl = 32;
  packet.ip.protocol = 1;
  mikos::copy_octets(packet.ip.source, peer_ip.octet);
  mikos::copy_octets(packet.ip.destination, guest_ip.octet);
  packet.icmp.type = 8;
  packet.icmp.identifier = mikos::net16(7);
  packet.icmp.sequence = mikos::net16(9);
  packet.payload[0] = 0xde;
  packet.payload[1] = 0xad;
  packet.payload[2] = 0xbe;
  packet.payload[3] = 0xef;
  packet.icmp.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&packet.icmp),
      sizeof(packet) - sizeof(packet.ethernet) - sizeof(packet.ip)));
  packet.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&packet.ip), sizeof(packet.ip)));
  return packet;
}

}  // namespace

int main() {
  mikos::test::Suite suite{"net/ethernet"};

  auto arp = arp_request();
  const auto arp_size = mikos::make_arp_reply(
      reinterpret_cast<mikos::u8*>(&arp), sizeof(arp), guest_mac, guest_ip);
  MIKOS_CHECK(suite, arp_size == sizeof(arp));
  MIKOS_CHECK(suite, arp.operation == mikos::net16(2));
  MIKOS_CHECK(suite, arp.ethernet.destination[5] == 2);
  MIKOS_CHECK(suite, arp.sender_mac[0] == 0x52);
  MIKOS_CHECK(suite, arp.sender_mac[5] == 0x56);

  auto wrong_arp = arp_request();
  wrong_arp.target_ip[3] = 99;
  MIKOS_CHECK(suite,
              mikos::make_arp_reply(reinterpret_cast<mikos::u8*>(&wrong_arp),
                                    sizeof(wrong_arp), guest_mac, guest_ip) ==
                  0);
  MIKOS_CHECK(suite,
              mikos::make_arp_reply(reinterpret_cast<mikos::u8*>(&wrong_arp),
                                    sizeof(wrong_arp) - 1, guest_mac,
                                    guest_ip) == 0);

  auto echo = echo_request();
  const auto echo_size = mikos::make_icmp_echo_reply(
      reinterpret_cast<mikos::u8*>(&echo), sizeof(echo), guest_mac, guest_ip);
  MIKOS_CHECK(suite, echo_size == sizeof(echo));
  MIKOS_CHECK(suite, echo.icmp.type == 0);
  MIKOS_CHECK(suite, echo.ip.ttl == 64);
  MIKOS_CHECK(suite,
              mikos::internet_checksum(
                  reinterpret_cast<const mikos::u8*>(&echo.ip),
                  sizeof(echo.ip)) == 0);
  MIKOS_CHECK(suite,
              mikos::internet_checksum(
                  reinterpret_cast<const mikos::u8*>(&echo.icmp),
                  sizeof(echo) - sizeof(echo.ethernet) - sizeof(echo.ip)) ==
                  0);

  auto corrupt_echo = echo_request();
  corrupt_echo.payload[0] ^= 1;
  MIKOS_CHECK(
      suite,
      mikos::make_icmp_echo_reply(
          reinterpret_cast<mikos::u8*>(&corrupt_echo), sizeof(corrupt_echo),
          guest_mac, guest_ip) == 0);

  return suite.finish();
}
