#pragma once

#include <mikos/base.hpp>

namespace mikos {

struct MacAddress {
  u8 octet[6];

  [[nodiscard]] constexpr bool operator==(const MacAddress&) const = default;
};

struct Ipv4Address {
  u8 octet[4];

  [[nodiscard]] constexpr bool operator==(const Ipv4Address&) const = default;
};

[[nodiscard]] constexpr u16 net16(u16 value) {
  return static_cast<u16>((value << 8) | (value >> 8));
}

template <u32 Size>
constexpr void copy_octets(u8 (&destination)[Size], const u8 (&source)[Size]) {
  for (u32 i = 0; i < Size; ++i) {
    destination[i] = source[i];
  }
}

struct [[gnu::packed]] EthernetHeader {
  u8 destination[6];
  u8 source[6];
  u16 type;
};

struct [[gnu::packed]] ArpIpv4 {
  EthernetHeader ethernet;
  u16 hardware_type;
  u16 protocol_type;
  u8 hardware_size;
  u8 protocol_size;
  u16 operation;
  u8 sender_mac[6];
  u8 sender_ip[4];
  u8 target_mac[6];
  u8 target_ip[4];
};

struct [[gnu::packed]] Ipv4Header {
  u8 version_ihl;
  u8 dscp_ecn;
  u16 total_length;
  u16 identification;
  u16 flags_fragment;
  u8 ttl;
  u8 protocol;
  u16 checksum;
  u8 source[4];
  u8 destination[4];
};

struct [[gnu::packed]] IcmpEchoHeader {
  u8 type;
  u8 code;
  u16 checksum;
  u16 identifier;
  u16 sequence;
};

static_assert(sizeof(EthernetHeader) == 14);
static_assert(sizeof(ArpIpv4) == 42);
static_assert(sizeof(Ipv4Header) == 20);
static_assert(sizeof(IcmpEchoHeader) == 8);

[[nodiscard]] constexpr u16 internet_checksum(const u8* bytes, u32 size) {
  u32 sum = 0;
  while (size >= 2) {
    sum += static_cast<u16>((static_cast<u16>(bytes[0]) << 8) | bytes[1]);
    bytes += 2;
    size -= 2;
  }
  if (size != 0) {
    sum += static_cast<u16>(bytes[0] << 8);
  }
  while ((sum >> 16) != 0) {
    sum = (sum & 0xffffu) + (sum >> 16);
  }
  return static_cast<u16>(~sum);
}

[[nodiscard]] constexpr u32 make_arp_reply(u8* bytes, u32 size,
                                            MacAddress local_mac,
                                            Ipv4Address local_ip) {
  if (size < sizeof(ArpIpv4)) {
    return 0;
  }
  auto& packet = *reinterpret_cast<ArpIpv4*>(bytes);
  if (packet.ethernet.type != net16(0x0806) ||
      packet.hardware_type != net16(1) ||
      packet.protocol_type != net16(0x0800) || packet.hardware_size != 6 ||
      packet.protocol_size != 4 || packet.operation != net16(1)) {
    return 0;
  }
  for (u32 i = 0; i < 4; ++i) {
    if (packet.target_ip[i] != local_ip.octet[i]) {
      return 0;
    }
  }

  u8 requester_mac[6]{};
  u8 requester_ip[4]{};
  copy_octets(requester_mac, packet.sender_mac);
  copy_octets(requester_ip, packet.sender_ip);
  copy_octets(packet.ethernet.destination, requester_mac);
  copy_octets(packet.ethernet.source, local_mac.octet);
  packet.operation = net16(2);
  copy_octets(packet.target_mac, requester_mac);
  copy_octets(packet.target_ip, requester_ip);
  copy_octets(packet.sender_mac, local_mac.octet);
  copy_octets(packet.sender_ip, local_ip.octet);
  return sizeof(ArpIpv4);
}

[[nodiscard]] constexpr u32 make_icmp_echo_reply(u8* bytes, u32 size,
                                                  MacAddress local_mac,
                                                  Ipv4Address local_ip) {
  constexpr u32 headers_size = sizeof(EthernetHeader) + sizeof(Ipv4Header) +
                               sizeof(IcmpEchoHeader);
  if (size < headers_size) {
    return 0;
  }
  auto& ethernet = *reinterpret_cast<EthernetHeader*>(bytes);
  auto& ip = *reinterpret_cast<Ipv4Header*>(bytes + sizeof(EthernetHeader));
  if (ethernet.type != net16(0x0800) || ip.version_ihl != 0x45 ||
      ip.protocol != 1 ||
      internet_checksum(reinterpret_cast<const u8*>(&ip), sizeof(ip)) != 0) {
    return 0;
  }
  for (u32 i = 0; i < 4; ++i) {
    if (ip.destination[i] != local_ip.octet[i]) {
      return 0;
    }
  }

  const u32 ip_size = net16(ip.total_length);
  if (ip_size < sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) ||
      ip_size > size - sizeof(EthernetHeader)) {
    return 0;
  }
  auto* icmp_bytes = bytes + sizeof(EthernetHeader) + sizeof(Ipv4Header);
  const u32 icmp_size = ip_size - sizeof(Ipv4Header);
  auto& icmp = *reinterpret_cast<IcmpEchoHeader*>(icmp_bytes);
  if (icmp.type != 8 || icmp.code != 0 ||
      internet_checksum(icmp_bytes, icmp_size) != 0) {
    return 0;
  }

  u8 requester_mac[6]{};
  u8 requester_ip[4]{};
  copy_octets(requester_mac, ethernet.source);
  copy_octets(requester_ip, ip.source);
  copy_octets(ethernet.destination, requester_mac);
  copy_octets(ethernet.source, local_mac.octet);
  copy_octets(ip.destination, requester_ip);
  copy_octets(ip.source, local_ip.octet);
  ip.ttl = 64;
  ip.checksum = 0;
  ip.checksum = net16(internet_checksum(
      reinterpret_cast<const u8*>(&ip), sizeof(Ipv4Header)));
  icmp.type = 0;
  icmp.checksum = 0;
  icmp.checksum = net16(internet_checksum(icmp_bytes, icmp_size));
  return sizeof(EthernetHeader) + ip_size;
}

}  // namespace mikos
