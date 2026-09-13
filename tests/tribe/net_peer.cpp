#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <mikos/net/ethernet.hpp>

namespace {

constexpr std::uint8_t message_hello = 1;
constexpr std::uint8_t message_frame = 2;

[[nodiscard]] bool send_frame(int socket_fd, const sockaddr_un& simulator,
                              socklen_t simulator_size, const void* frame,
                              std::size_t size) {
  std::vector<std::uint8_t> message(size + 3);
  message[0] = message_frame;
  message[1] = static_cast<std::uint8_t>(size >> 8);
  message[2] = static_cast<std::uint8_t>(size);
  std::memcpy(message.data() + 3, frame, size);
  const ssize_t sent = sendto(socket_fd, message.data(), message.size(), 0,
                             reinterpret_cast<const sockaddr*>(&simulator),
                             simulator_size);
  if (sent != static_cast<ssize_t>(message.size())) {
    return false;
  }
  return true;
}

[[nodiscard]] bool receive_frame(int socket_fd,
                                 std::vector<std::uint8_t>& frame,
                                 int timeout_ms) {
  pollfd descriptor{socket_fd, POLLIN, 0};
  if (poll(&descriptor, 1, timeout_ms) <= 0 ||
      (descriptor.revents & POLLIN) == 0) {
    return false;
  }
  std::array<std::uint8_t, 4096> message{};
  const ssize_t received = recv(socket_fd, message.data(), message.size(), 0);
  if (received < 3 || message[0] != message_frame) {
    return false;
  }
  const std::size_t size =
      (static_cast<std::size_t>(message[1]) << 8) | message[2];
  if (size > static_cast<std::size_t>(received) - 3) {
    return false;
  }
  frame.assign(message.begin() + 3, message.begin() + 3 + size);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: net_peer SOCKET\n";
    return 2;
  }
  const int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return 2;
  }
  sockaddr_un local{};
  local.sun_family = AF_UNIX;
  if (std::strlen(argv[1]) >= sizeof(local.sun_path)) {
    return 2;
  }
  std::strcpy(local.sun_path, argv[1]);
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    std::cerr << "bind: " << std::strerror(errno) << '\n';
    return 2;
  }
  std::array<std::uint8_t, 16> hello{};
  sockaddr_un simulator{};
  socklen_t simulator_size = sizeof(simulator);
  const ssize_t hello_size =
      recvfrom(socket_fd, hello.data(), hello.size(), 0,
               reinterpret_cast<sockaddr*>(&simulator), &simulator_size);
  if (hello_size != 1 || hello[0] != message_hello) {
    std::cerr << "did not receive Tribe media hello\n";
    return 1;
  }

  const mikos::MacAddress peer_mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
  const mikos::MacAddress guest_mac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};
  const mikos::Ipv4Address peer_ip{{10, 0, 2, 2}};
  const mikos::Ipv4Address guest_ip{{10, 0, 2, 15}};

  mikos::ArpIpv4 arp{};
  for (auto& octet : arp.ethernet.destination) {
    octet = 0xff;
  }
  mikos::copy_octets(arp.ethernet.source, peer_mac.octet);
  arp.ethernet.type = mikos::net16(0x0806);
  arp.hardware_type = mikos::net16(1);
  arp.protocol_type = mikos::net16(0x0800);
  arp.hardware_size = 6;
  arp.protocol_size = 4;
  arp.operation = mikos::net16(1);
  mikos::copy_octets(arp.sender_mac, peer_mac.octet);
  mikos::copy_octets(arp.sender_ip, peer_ip.octet);
  mikos::copy_octets(arp.target_ip, guest_ip.octet);
  std::vector<std::uint8_t> received;
  bool arp_ok = false;
  for (unsigned attempt = 0; attempt < 480 && !arp_ok; ++attempt) {
    if (!send_frame(socket_fd, simulator, simulator_size, &arp, sizeof(arp))) {
      return 1;
    }
    if (!receive_frame(socket_fd, received, 500) ||
        received.size() < sizeof(arp)) {
      continue;
    }
    const auto& reply =
        *reinterpret_cast<const mikos::ArpIpv4*>(received.data());
    arp_ok = reply.operation == mikos::net16(2) &&
             std::memcmp(reply.sender_mac, guest_mac.octet, 6) == 0 &&
             std::memcmp(reply.sender_ip, guest_ip.octet, 4) == 0;
  }
  if (!arp_ok) {
    std::cerr << "missing ARP reply\n";
    return 1;
  }

  struct [[gnu::packed]] EchoPacket {
    mikos::EthernetHeader ethernet;
    mikos::Ipv4Header ip;
    mikos::IcmpEchoHeader icmp;
    // Exceed the former 256-byte MAC buffer to exercise full-frame reception.
    mikos::u8 payload[512];
  } echo{};
  mikos::copy_octets(echo.ethernet.source, peer_mac.octet);
  mikos::copy_octets(echo.ethernet.destination, guest_mac.octet);
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
  for (unsigned i = 0; i < sizeof(echo.payload); ++i) {
    echo.payload[i] = static_cast<mikos::u8>(i * 37u + 11u);
  }
  echo.icmp.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&echo.icmp),
      sizeof(echo) - sizeof(echo.ethernet) - sizeof(echo.ip)));
  echo.ip.checksum = mikos::net16(mikos::internet_checksum(
      reinterpret_cast<const mikos::u8*>(&echo.ip), sizeof(echo.ip)));
  constexpr auto icmp_size =
      sizeof(EchoPacket) - sizeof(mikos::EthernetHeader) -
      sizeof(mikos::Ipv4Header);
  bool echo_ok = false;
  for (unsigned attempt = 0; attempt < 480 && !echo_ok; ++attempt) {
    received.clear();
    // Drain already queued replies before retrying. The simulator may have
    // closed its socket after transmitting the valid final datagram.
    if (!receive_frame(socket_fd, received, 0)) {
      const bool sent = send_frame(socket_fd, simulator, simulator_size, &echo,
                                   sizeof(echo));
      if (!receive_frame(socket_fd, received, 500)) {
        if (!sent) {
          std::cerr << "simulator closed before a valid echo reply was received\n";
          return 1;
        }
        continue;
      }
    }
    if (received.size() < sizeof(echo)) {
      if (received.size() >= 14 && received[12] == 8 && received[13] == 0)
        std::cerr << "short IPv4 reply: " << received.size() << " bytes\n";
      continue;
    }
    const auto& reply = *reinterpret_cast<const EchoPacket*>(received.data());
    echo_ok = reply.ethernet.type == mikos::net16(0x0800) &&
              reply.ip.version_ihl == 0x45 && reply.ip.protocol == 1 &&
              reply.ip.total_length == echo.ip.total_length &&
              reply.icmp.type == 0 && reply.icmp.code == 0 &&
              reply.icmp.identifier == echo.icmp.identifier &&
              reply.icmp.sequence == echo.icmp.sequence &&
              std::memcmp(reply.ethernet.source, guest_mac.octet, 6) == 0 &&
              std::memcmp(reply.ip.source, guest_ip.octet, 4) == 0 &&
              std::memcmp(reply.ip.destination, peer_ip.octet, 4) == 0 &&
              mikos::internet_checksum(
                  reinterpret_cast<const mikos::u8*>(&reply.ip),
                  sizeof(reply.ip)) == 0 &&
              mikos::internet_checksum(
                  reinterpret_cast<const mikos::u8*>(&reply.icmp),
                  icmp_size) == 0 &&
              std::memcmp(reply.payload, echo.payload,
                          sizeof(echo.payload)) == 0;
    if (!echo_ok) {
      for (unsigned i = 0; i < sizeof(echo.payload); ++i) {
        if (reply.payload[i] != echo.payload[i]) {
          std::cerr << "first payload mismatch at " << i << ": got "
                    << unsigned(reply.payload[i]) << " expected " << unsigned(echo.payload[i]) << '\n';
          break;
        }
      }
      std::cerr << "invalid echo reply: bytes=" << received.size()
                << " ip_length=" << mikos::net16(reply.ip.total_length)
                << " type=" << unsigned(reply.icmp.type)
                << " ip_checksum=" << mikos::internet_checksum(
                     reinterpret_cast<const mikos::u8*>(&reply.ip), sizeof(reply.ip))
                << " icmp_checksum=" << mikos::internet_checksum(
                     reinterpret_cast<const mikos::u8*>(&reply.icmp), icmp_size)
                << " payload_match=" << (std::memcmp(reply.payload, echo.payload,
                                                    sizeof(echo.payload)) == 0) << '\n';
    }
  }
  if (!echo_ok) {
    std::cerr << "missing valid IPv4 ICMP echo reply\n";
    return 1;
  }

  std::cout << "PASS: IPv4 ping with 512-byte payload received from MikOS on Tribe\n";
  close(socket_fd);
  return 0;
}
