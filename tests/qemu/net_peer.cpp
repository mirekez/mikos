#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>

#include <mikos/net/ethernet.hpp>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: net_peer LOCAL_SOCKET QEMU_SOCKET\n";
    return 2;
  }
  const int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return 2;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (std::strlen(argv[1]) >= sizeof(address.sun_path)) {
    return 2;
  }
  std::strcpy(address.sun_path, argv[1]);
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    std::cerr << std::format("bind: {}\n", std::strerror(errno));
    return 2;
  }
  sockaddr_un qemu{};
  qemu.sun_family = AF_UNIX;
  if (std::strlen(argv[2]) >= sizeof(qemu.sun_path)) {
    return 2;
  }
  std::strcpy(qemu.sun_path, argv[2]);
  timeval receive_timeout{5, 0};
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
             sizeof(receive_timeout));

  mikos::ArpIpv4 request{};
  const mikos::MacAddress peer_mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
  const mikos::Ipv4Address peer_ip{{10, 0, 2, 2}};
  const mikos::Ipv4Address guest_ip{{10, 0, 2, 15}};
  for (auto& octet : request.ethernet.destination) {
    octet = 0xff;
  }
  mikos::copy_octets(request.ethernet.source, peer_mac.octet);
  request.ethernet.type = mikos::net16(0x0806);
  request.hardware_type = mikos::net16(1);
  request.protocol_type = mikos::net16(0x0800);
  request.hardware_size = 6;
  request.protocol_size = 4;
  request.operation = mikos::net16(1);
  mikos::copy_octets(request.sender_mac, peer_mac.octet);
  mikos::copy_octets(request.sender_ip, peer_ip.octet);
  mikos::copy_octets(request.target_ip, guest_ip.octet);

  if (sendto(socket_fd, &request, sizeof(request), 0,
             reinterpret_cast<sockaddr*>(&qemu), sizeof(qemu)) !=
      static_cast<ssize_t>(sizeof(request))) {
    std::cerr << std::format("sendto: {}\n", std::strerror(errno));
    return 1;
  }

  mikos::ArpIpv4 reply{};
  if (recv(socket_fd, &reply, sizeof(reply), 0) !=
      static_cast<ssize_t>(sizeof(reply))) {
    std::cerr << std::format("recv: {}\n", std::strerror(errno));
    return 1;
  }
  const mikos::MacAddress expected_guest{{0x52, 0x54, 0x00, 0x12, 0x34,
                                           0x56}};
  if (reply.operation != mikos::net16(2) ||
      std::memcmp(reply.sender_mac, expected_guest.octet, 6) != 0 ||
      std::memcmp(reply.sender_ip, guest_ip.octet, 4) != 0 ||
      std::memcmp(reply.target_mac, peer_mac.octet, 6) != 0) {
    std::cerr << "invalid ARP reply\n";
    return 1;
  }
  std::cout << "PASS: host received guest ARP reply\n";

  struct [[gnu::packed]] EchoPacket {
    mikos::EthernetHeader ethernet;
    mikos::Ipv4Header ip;
    mikos::IcmpEchoHeader icmp;
    mikos::u8 payload[4];
  } echo{};
  mikos::copy_octets(echo.ethernet.source, peer_mac.octet);
  mikos::copy_octets(echo.ethernet.destination, expected_guest.octet);
  echo.ethernet.type = mikos::net16(0x0800);
  echo.ip.version_ihl = 0x45;
  echo.ip.total_length =
      mikos::net16(sizeof(echo) - sizeof(mikos::EthernetHeader));
  echo.ip.identification = mikos::net16(0x1234);
  echo.ip.ttl = 32;
  echo.ip.protocol = 1;
  mikos::copy_octets(echo.ip.source, peer_ip.octet);
  mikos::copy_octets(echo.ip.destination, guest_ip.octet);
  echo.icmp.type = 8;
  echo.icmp.identifier = mikos::net16(7);
  echo.icmp.sequence = mikos::net16(9);
  echo.payload[0] = 0xde;
  echo.payload[1] = 0xad;
  echo.payload[2] = 0xbe;
  echo.payload[3] = 0xef;
  echo.icmp.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&echo.icmp),
      sizeof(echo) - sizeof(echo.ethernet) - sizeof(echo.ip)));
  echo.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&echo.ip), sizeof(echo.ip)));

  if (sendto(socket_fd, &echo, sizeof(echo), 0,
             reinterpret_cast<sockaddr*>(&qemu), sizeof(qemu)) !=
      static_cast<ssize_t>(sizeof(echo))) {
    std::cerr << std::format("sendto ICMP: {}\n", std::strerror(errno));
    return 1;
  }
  EchoPacket echo_reply{};
  if (recv(socket_fd, &echo_reply, sizeof(echo_reply), 0) !=
      static_cast<ssize_t>(sizeof(echo_reply))) {
    std::cerr << std::format("recv ICMP: {}\n", std::strerror(errno));
    return 1;
  }
  const auto icmp_size = sizeof(echo_reply) - sizeof(echo_reply.ethernet) -
                         sizeof(echo_reply.ip);
  if (echo_reply.icmp.type != 0 || echo_reply.icmp.code != 0 ||
      std::memcmp(echo_reply.ethernet.source, expected_guest.octet, 6) != 0 ||
      std::memcmp(echo_reply.ip.source, guest_ip.octet, 4) != 0 ||
      std::memcmp(echo_reply.ip.destination, peer_ip.octet, 4) != 0 ||
      mikos::internet_checksum(
          reinterpret_cast<const mikos::u8*>(&echo_reply.ip),
          sizeof(echo_reply.ip)) != 0 ||
      mikos::internet_checksum(
          reinterpret_cast<const mikos::u8*>(&echo_reply.icmp), icmp_size) !=
          0 ||
      std::memcmp(echo_reply.payload, echo.payload, sizeof(echo.payload)) != 0) {
    std::cerr << "invalid ICMP echo reply\n";
    return 1;
  }
  std::cout << "PASS: host received guest ICMP echo reply\n";
  return 0;
}
