#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <mikos/net/ethernet.hpp>
#include <optional>
#include <span>

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

[[nodiscard]] constexpr u32 net32(u32 value) { return std::byteswap(value); }

[[nodiscard]] constexpr u32 checksum_add(u32 sum, const u8* bytes, u32 size) {
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
  const std::array<u8, 4> pseudo{0, 6, static_cast<u8>(size >> 8),
                                 static_cast<u8>(size)};
  sum = checksum_add(sum, pseudo.data(), pseudo.size());
  return checksum_finish(checksum_add(sum, segment, size));
}

struct TcpOptions {
  std::optional<u16> maximum_segment_size;
  std::optional<u8> window_scale;
};

[[nodiscard]] constexpr std::optional<TcpOptions> parse_tcp_options(
    std::span<const u8> bytes) {
  TcpOptions result;
  while (!bytes.empty()) {
    const auto kind = bytes.front();
    if (kind == 0) break;  // End of list; the remaining bytes are padding.
    if (kind == 1) {
      bytes = bytes.subspan(1);
      continue;
    }
    if (bytes.size() < 2 || bytes[1] < 2 || bytes[1] > bytes.size())
      return std::nullopt;
    const auto option = bytes.first(bytes[1]);
    switch (kind) {
      case 2:
        if (option.size() != 4 || result.maximum_segment_size)
          return std::nullopt;
        result.maximum_segment_size =
            static_cast<u16>((u16{option[2]} << 8) | option[3]);
        if (*result.maximum_segment_size == 0) return std::nullopt;
        break;
      case 3:
        if (option.size() != 3 || result.window_scale) return std::nullopt;
        result.window_scale = std::min<u8>(option[2], 14);
        break;
      case 4:
        if (option.size() != 2) return std::nullopt;
        break;
      case 5:
        if (option.size() < 10 || (option.size() - 2) % 8 != 0)
          return std::nullopt;
        break;
      case 8:
        if (option.size() != 10) return std::nullopt;
        break;
      default:
        break;  // Unknown, correctly framed extensions are safely skipped.
    }
    bytes = bytes.subspan(option.size());
  }
  return result;
}

// Header values are owned; only the read-only payload borrows packet storage.
struct TcpView {
  EthernetHeader ethernet{};
  Ipv4Header ip{};
  TcpHeader tcp{};
  std::span<const u8> payload{};
  TcpOptions options{};
};

namespace detail {

template <typename Header>
[[nodiscard]] constexpr Header decode_header(
    std::span<const u8, sizeof(Header)> bytes) {
  std::array<u8, sizeof(Header)> representation{};
  std::ranges::copy(bytes, representation.begin());
  return std::bit_cast<Header>(representation);
}

}  // namespace detail

[[nodiscard]] constexpr std::optional<TcpView> parse_tcp(
    std::span<const u8> bytes, Ipv4Address local_ip) {
  constexpr auto minimum =
      sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(TcpHeader);
  if (bytes.size() < minimum) {
    return std::nullopt;
  }
  const auto ethernet = detail::decode_header<EthernetHeader>(
      bytes.first<sizeof(EthernetHeader)>());
  const auto ip_bytes = bytes.subspan(sizeof(EthernetHeader));
  const auto ip =
      detail::decode_header<Ipv4Header>(ip_bytes.first<sizeof(Ipv4Header)>());
  const auto ip_header_size =
      static_cast<std::size_t>(ip.version_ihl & 0x0f) * 4;
  const auto ip_size = net16(ip.total_length);
  // Validate the advertised lengths before reading options or checksumming.
  if (ethernet.type != net16(0x0800) || (ip.version_ihl >> 4) != 4 ||
      ip_header_size < sizeof(Ipv4Header) || ip_header_size > ip_bytes.size() ||
      ip_size < ip_header_size + sizeof(TcpHeader) ||
      ip_size > ip_bytes.size() || ip.protocol != 6 ||
      (net16(ip.flags_fragment) & 0x3fffu) != 0 ||
      !std::ranges::equal(ip.destination, local_ip.octet) ||
      internet_checksum(ip_bytes.data(), ip_header_size) != 0) {
    return std::nullopt;
  }
  const auto tcp_bytes =
      ip_bytes.subspan(ip_header_size, ip_size - ip_header_size);
  const auto tcp =
      detail::decode_header<TcpHeader>(tcp_bytes.first<sizeof(TcpHeader)>());
  const auto tcp_header_size =
      static_cast<std::size_t>(tcp.data_offset >> 4) * 4;
  const Ipv4Address source{
      {ip.source[0], ip.source[1], ip.source[2], ip.source[3]}};
  if (tcp_header_size < sizeof(TcpHeader) ||
      tcp_header_size > tcp_bytes.size() ||
      tcp_checksum(source, local_ip, tcp_bytes.data(), tcp_bytes.size()) != 0) {
    return std::nullopt;
  }
  const auto options = parse_tcp_options(tcp_bytes.subspan(
      sizeof(TcpHeader), tcp_header_size - sizeof(TcpHeader)));
  if (!options) return std::nullopt;
  return TcpView{ethernet, ip, tcp, tcp_bytes.subspan(tcp_header_size),
                 *options};
}

}  // namespace mikos
