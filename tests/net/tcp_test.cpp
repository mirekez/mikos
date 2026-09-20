#include <mikos/net/tcp.hpp>

#include <support/test.hpp>
#include <type_traits>

namespace {

constexpr mikos::Ipv4Address local_ip{{192, 168, 76, 2}};
constexpr mikos::Ipv4Address peer_ip{{192, 168, 76, 1}};

// Wire fixtures with independently calculated IP/TCP checksums. Keeping them
// as bytes also makes the parser test independent of C++ struct layout.
constexpr std::array<mikos::u8, 58> packet{
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x45, 0x00, 0x00, 0x2c, 0x00, 0x07, 0x00, 0x00, 0x40, 0x06,
    0x61, 0x71, 0xc0, 0xa8, 0x4c, 0x01, 0xc0, 0xa8, 0x4c, 0x02, 0xc0, 0x00,
    0x00, 0x16, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x13, 0x89, 0x50, 0x18,
    0x10, 0x00, 0xc7, 0x12, 0x00, 0x00, 0x74, 0x65, 0x73, 0x74,
};
constexpr std::array<mikos::u8, 66> options_packet{
    0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x46, 0x00, 0x00, 0x34, 0x00, 0x07, 0x00, 0x00, 0x40, 0x06,
    0x60, 0x69, 0xc0, 0xa8, 0x4c, 0x01, 0xc0, 0xa8, 0x4c, 0x02, 0x00, 0x00,
    0x00, 0x00, 0xc0, 0x00, 0x00, 0x16, 0x00, 0x00, 0x03, 0xe8, 0x00, 0x00,
    0x13, 0x89, 0x60, 0x18, 0x10, 0x00, 0xb6, 0x0d, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x74, 0x65, 0x73, 0x74,
};

// Parsing really is constexpr, including bit_cast and checksum validation.
constexpr auto decoded = mikos::parse_tcp(packet, local_ip);
static_assert(decoded.has_value());
static_assert(decoded->tcp.source_port == mikos::net16(49152));
static_assert(decoded->payload.size() == 4 && decoded->payload[0] == 't');
static_assert(mikos::parse_tcp(options_packet, local_ip)->payload.size() == 4);
static_assert(std::is_same_v<decltype(mikos::TcpView::payload),
                             std::span<const mikos::u8>>);

void repair_ip_checksum(std::span<mikos::u8> bytes) {
  bytes[24] = bytes[25] = 0;
  const auto checksum = mikos::internet_checksum(bytes.data() + 14, 20);
  bytes[24] = static_cast<mikos::u8>(checksum >> 8);
  bytes[25] = static_cast<mikos::u8>(checksum);
}

}  // namespace

int main() {
  using namespace mikos;
  test::Suite suite{"net/tcp"};
  const auto valid = parse_tcp(packet, local_ip);
  MIKOS_CHECK(suite, valid.has_value());
  if (!valid) return suite.finish();
  MIKOS_CHECK(suite, valid->tcp.source_port == net16(49152));
  MIKOS_CHECK(suite, valid->tcp.destination_port == net16(22));
  MIKOS_CHECK(suite, net32(valid->tcp.sequence) == 1000);
  MIKOS_CHECK(suite, net32(valid->tcp.acknowledgement) == 5001);
  MIKOS_CHECK(suite, valid->payload.size() == 4);
  MIKOS_CHECK(suite, valid->payload[0] == 't' && valid->payload[3] == 't');
  MIKOS_CHECK(suite, tcp_checksum(peer_ip, local_ip, packet.data() + 34, 24) == 0);

  const auto options = parse_tcp(options_packet, local_ip);
  MIKOS_CHECK(suite, options.has_value());
  if (options) {
    MIKOS_CHECK(suite, options->ip.version_ihl == 0x46);
    MIKOS_CHECK(suite, options->tcp.data_offset == 0x60);
    MIKOS_CHECK(suite, std::ranges::equal(options->payload, valid->payload));
    MIKOS_CHECK(suite, options->payload.data() == options_packet.data() + 62);
  }

  // Every truncation must fail, including partial IP/TCP option areas.
  for (const auto bytes : {std::span<const u8>{packet},
                           std::span<const u8>{options_packet}}) {
    for (std::size_t size = 0; size < bytes.size(); ++size) {
      MIKOS_CHECK(suite, !parse_tcp(bytes.first(size), local_ip));
    }
  }
  MIKOS_CHECK(suite, !parse_tcp({}, local_ip));

  // An IPv4 header can claim 60 bytes even in a minimum-length frame.
  // Length validation must happen before the checksum reads those bytes.
  std::array<u8, 54> truncated_options{};
  std::ranges::copy(std::span{packet}.first<54>(), truncated_options.begin());
  truncated_options[14] = 0x4f;
  MIKOS_CHECK(suite, !parse_tcp(truncated_options, local_ip));

  alignas(8) std::array<u8, packet.size() + 1> unaligned{};
  std::ranges::copy(packet, unaligned.begin() + 1);
  const auto unaligned_result = parse_tcp(std::span{unaligned}.subspan(1), local_ip);
  MIKOS_CHECK(suite, unaligned_result.has_value());
  if (unaligned_result) {
    MIKOS_CHECK(suite, unaligned_result->tcp.sequence == valid->tcp.sequence);
    MIKOS_CHECK(suite, std::ranges::equal(unaligned_result->payload, valid->payload));
  }

  std::array<u8, 80> padded{};
  std::ranges::fill(padded, 0xff);
  std::ranges::copy(packet, padded.begin());
  const auto padded_result = parse_tcp(padded, local_ip);
  MIKOS_CHECK(suite, padded_result.has_value());
  if (padded_result) {
    MIKOS_CHECK(suite, padded_result->payload.size() == 4);
  }

  // Header values survive changes to receive storage; payload remains a view.
  auto mutable_packet = packet;
  const auto saved = parse_tcp(mutable_packet, local_ip);
  mutable_packet[34] = 0;
  mutable_packet[54] = 'X';
  MIKOS_CHECK(suite, saved->tcp.source_port == net16(49152));
  MIKOS_CHECK(suite, saved->payload[0] == 'X');

  auto wrong_destination = packet;
  wrong_destination[33] = 99;
  repair_ip_checksum(wrong_destination);
  MIKOS_CHECK(suite, !parse_tcp(wrong_destination, local_ip));
  auto corrupt_ip = packet;
  corrupt_ip[22] ^= 1;
  MIKOS_CHECK(suite, !parse_tcp(corrupt_ip, local_ip));
  auto corrupt_tcp = packet;
  corrupt_tcp.back() ^= 1;
  MIKOS_CHECK(suite, !parse_tcp(corrupt_tcp, local_ip));
  auto fragment = packet;
  fragment[21] = 1;
  repair_ip_checksum(fragment);
  MIKOS_CHECK(suite, !parse_tcp(fragment, local_ip));
  auto short_ip_total = packet;
  short_ip_total[17] = 39;
  repair_ip_checksum(short_ip_total);
  MIKOS_CHECK(suite, !parse_tcp(short_ip_total, local_ip));
  auto oversized_ip_total = packet;
  oversized_ip_total[16] = 0xff;
  oversized_ip_total[17] = 0xff;
  repair_ip_checksum(oversized_ip_total);
  MIKOS_CHECK(suite, !parse_tcp(oversized_ip_total, local_ip));
  auto bad_tcp_offset = packet;
  bad_tcp_offset[46] = 4 << 4;
  MIKOS_CHECK(suite, !parse_tcp(bad_tcp_offset, local_ip));
  bad_tcp_offset[46] = 15 << 4;
  MIKOS_CHECK(suite, !parse_tcp(bad_tcp_offset, local_ip));
  auto wrong_protocol = packet;
  wrong_protocol[23] = 17;
  repair_ip_checksum(wrong_protocol);
  MIKOS_CHECK(suite, !parse_tcp(wrong_protocol, local_ip));

  return suite.finish();
}
