#pragma once

#include <mikos/net/ethernet.hpp>

namespace mikos {

struct [[gnu::packed]] TcpHeader {
  u16 source_port;
  u16 destination_port;
  u32 sequence;
  u32 acknowledgement;
  u8 data_offset;
  u8 flags;
  u16 window;
  u16 checksum;
  u16 urgent;
};

static_assert(sizeof(TcpHeader) == 20);

inline constexpr u8 tcp_fin = 0x01;
inline constexpr u8 tcp_syn = 0x02;
inline constexpr u8 tcp_rst = 0x04;
inline constexpr u8 tcp_psh = 0x08;
inline constexpr u8 tcp_ack = 0x10;

[[nodiscard]] constexpr u32 net32(u32 value) {
  return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
         ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
}

[[nodiscard]] constexpr u32 checksum_add(u32 sum, const u8* bytes,
                                         u32 size) {
  while (size >= 2) {
    sum += static_cast<u16>((static_cast<u16>(bytes[0]) << 8) | bytes[1]);
    bytes += 2;
    size -= 2;
  }
  return size == 0 ? sum : sum + static_cast<u16>(bytes[0] << 8);
}

[[nodiscard]] constexpr u16 checksum_finish(u32 sum) {
  while ((sum >> 16) != 0) {
    sum = (sum & 0xffffu) + (sum >> 16);
  }
  return static_cast<u16>(~sum);
}

[[nodiscard]] constexpr u16 tcp_checksum(Ipv4Address source,
                                         Ipv4Address destination,
                                         const u8* segment, u32 size) {
  u32 sum = checksum_add(0, source.octet, 4);
  sum = checksum_add(sum, destination.octet, 4);
  const u8 pseudo[4]{0, 6, static_cast<u8>(size >> 8),
                     static_cast<u8>(size)};
  sum = checksum_add(sum, pseudo, sizeof(pseudo));
  return checksum_finish(checksum_add(sum, segment, size));
}

struct TcpView {
  EthernetHeader* ethernet{};
  Ipv4Header* ip{};
  TcpHeader* tcp{};
  u8* payload{};
  u32 payload_size{};
};

[[nodiscard]] constexpr bool parse_tcp(u8* bytes, u32 size,
                                       Ipv4Address local_ip, TcpView& view) {
  constexpr u32 minimum = sizeof(EthernetHeader) + sizeof(Ipv4Header) +
                          sizeof(TcpHeader);
  if (size < minimum) {
    return false;
  }
  auto* ethernet = reinterpret_cast<EthernetHeader*>(bytes);
  auto* ip = reinterpret_cast<Ipv4Header*>(bytes + sizeof(EthernetHeader));
  const u32 ip_header_size = static_cast<u32>(ip->version_ihl & 0x0f) * 4;
  if (ethernet->type != net16(0x0800) || (ip->version_ihl >> 4) != 4 ||
      ip_header_size < sizeof(Ipv4Header) || ip_header_size > 60 ||
      ip->protocol != 6 ||
      internet_checksum(reinterpret_cast<const u8*>(ip), ip_header_size) != 0) {
    return false;
  }
  for (u32 i = 0; i < 4; ++i) {
    if (ip->destination[i] != local_ip.octet[i]) {
      return false;
    }
  }
  const u32 ip_size = net16(ip->total_length);
  if (ip_size < ip_header_size + sizeof(TcpHeader) ||
      ip_size > size - sizeof(EthernetHeader) ||
      (net16(ip->flags_fragment) & 0x3fffu) != 0) {
    return false;
  }
  auto* tcp = reinterpret_cast<TcpHeader*>(
      bytes + sizeof(EthernetHeader) + ip_header_size);
  const u32 tcp_size = ip_size - ip_header_size;
  const u32 tcp_header_size = static_cast<u32>(tcp->data_offset >> 4) * 4;
  const Ipv4Address source{{ip->source[0], ip->source[1], ip->source[2],
                            ip->source[3]}};
  if (tcp_header_size < sizeof(TcpHeader) || tcp_header_size > tcp_size ||
      tcp_checksum(source, local_ip, reinterpret_cast<const u8*>(tcp),
                   tcp_size) != 0) {
    return false;
  }
  view = {ethernet, ip, tcp, reinterpret_cast<u8*>(tcp) + tcp_header_size,
          tcp_size - tcp_header_size};
  return true;
}

}  // namespace mikos
