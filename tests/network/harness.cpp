#include "harness.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <drivers/net/net.hpp>

namespace tcp_test {
u64 now{};
std::deque<Bytes> incoming;
std::vector<Segment> outgoing;
unsigned fail_transmits{};
namespace {
Bytes receiving;
}

void require(bool passed, std::string_view expression, int line) {
  if (!passed) {
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    std::exit(1);
  }
}
u16 get16(std::span<const u8> b, std::size_t offset) {
  return (u16{b[offset]} << 8) | b[offset + 1];
}
u32 get32(std::span<const u8> b, std::size_t offset) {
  return (u32{get16(b, offset)} << 16) | get16(b, offset + 2);
}
void put16(Bytes& b, std::size_t offset, u16 value) {
  b[offset] = value >> 8;
  b[offset + 1] = value;
}
void put32(Bytes& b, std::size_t offset, u32 value) {
  put16(b, offset, value >> 16);
  put16(b, offset + 2, value);
}
u16 checksum(std::span<const u8> bytes) {
  u32 sum = 0;
  for (std::size_t i = 0; i < bytes.size(); i += 2) {
    sum += u32{bytes[i]} << 8;
    if (i + 1 < bytes.size()) sum += bytes[i + 1];
  }
  while (sum > 65535) sum = (sum & 65535) + (sum >> 16);
  return static_cast<u16>(~sum);
}
Bytes pseudo(std::span<const u8> frame) {
  const auto ip_size = get16(frame, 16);
  const auto ip_header = (frame[14] & 15) * 4;
  Bytes result(frame.begin() + 26, frame.begin() + 34);
  result.insert(result.end(),
                {0, 6, static_cast<u8>((ip_size - ip_header) >> 8),
                 static_cast<u8>(ip_size - ip_header)});
  result.insert(result.end(), frame.begin() + 14 + ip_header,
                frame.begin() + 14 + ip_size);
  return result;
}
Bytes packet(u16 port, u8 flags, u32 sequence, u32 acknowledgement, u16 window,
             std::span<const u8> data, std::span<const u8> options) {
  REQUIRE(options.size() <= 40 && options.size() % 4 == 0);
  Bytes frame(54 + options.size() + data.size());
  frame[0] = frame[6] = 2;
  frame[5] = 2;
  frame[11] = 1;
  put16(frame, 12, 0x0800);
  frame[14] = 0x45;
  put16(frame, 16, frame.size() - 14);
  frame[22] = 64;
  frame[23] = 6;
  std::ranges::copy(peer_ip.octet, frame.begin() + 26);
  std::ranges::copy(guest_ip.octet, frame.begin() + 30);
  put16(frame, 34, port);
  put16(frame, 36, 22);
  put32(frame, 38, sequence);
  put32(frame, 42, acknowledgement);
  frame[46] = (5 + options.size() / 4) << 4;
  frame[47] = flags;
  put16(frame, 48, window);
  std::ranges::copy(options, frame.begin() + 54);
  std::ranges::copy(data, frame.begin() + 54 + options.size());
  put16(frame, 50, checksum(pseudo(frame)));
  put16(frame, 24, checksum(std::span{frame}.subspan(14, 20)));
  return frame;
}
void record(std::span<const u8> frame) {
  REQUIRE(frame.size() >= 54);
  REQUIRE(get16(frame, 12) == 0x0800 && frame[23] == 6);
  REQUIRE(checksum(frame.subspan(14, (frame[14] & 15) * 4)) == 0);
  REQUIRE(get16(frame, 16) + 14u == frame.size());
  REQUIRE(checksum(pseudo(frame)) == 0);
  const std::size_t tcp = 14 + (frame[14] & 15) * 4;
  const std::size_t header = (frame[tcp + 12] >> 4) * 4;
  REQUIRE(header >= 20 && tcp + header <= frame.size());
  outgoing.push_back(
      {get32(frame, tcp + 4), get32(frame, tcp + 8), get16(frame, tcp + 2),
       get16(frame, tcp + 14), frame[tcp + 13],
       Bytes(frame.begin() + tcp + 20, frame.begin() + tcp + header),
       Bytes(frame.begin() + tcp + header, frame.end())});
}
Harness::Harness() {
  REQUIRE(initialize());
  const auto opened = socket_open(abi::socket::Type::stream);
  REQUIRE(opened.result == SocketResult::success);
  listener = opened.handle;
  REQUIRE(socket_bind(listener, {guest_ip, 22}) == SocketResult::success);
  REQUIRE(socket_listen(listener, listen_backlog_capacity) ==
          SocketResult::success);
}
Session Harness::connect(u16 window, std::span<const u8> options, u16 port,
                         u32 sequence) {
  Session s{invalid_socket, port, sequence + 1, 0};
  send(s, tcp_syn, sequence, 0, window, {}, options);
  REQUIRE(!outgoing.empty() && outgoing.back().flags == (tcp_syn | tcp_ack));
  s.server_next = outgoing.back().sequence + 1;
  ack(s, s.server_next, window);
  const auto accepted = socket_accept(listener);
  REQUIRE(accepted.result == SocketResult::success &&
          accepted.peer.port == port);
  s.handle = accepted.handle;
  clear();
  return s;
}
void Harness::receive(Bytes frame) {
  incoming.push_back(std::move(frame));
  poll();
}
void Harness::send(const Session& s, u8 flags, u32 sequence, u32 ack,
                   u16 window, std::span<const u8> data,
                   std::span<const u8> options) {
  receive(packet(s.port, flags, sequence, ack, window, data, options));
}
void Harness::advance(u64 milliseconds) { ticks(milliseconds * ticks_per_ms); }
void Harness::ticks(u64 count) {
  now += count;
  poll();
}
void Harness::idle(unsigned polls) {
  while (polls-- != 0) poll();
}
void Harness::clear() { outgoing.clear(); }
std::vector<Segment> Harness::data() const {
  std::vector<Segment> result;
  std::ranges::copy_if(
      outgoing, std::back_inserter(result),
      [](const auto& segment) { return !segment.payload.empty(); });
  return result;
}
Bytes Harness::read(const Session& s, std::size_t count) {
  Bytes output(count);
  const auto result = socket_read(s.handle, output.data(), output.size());
  REQUIRE(result.result == SocketResult::success);
  output.resize(result.size);
  return output;
}
Bytes pattern(std::size_t count, unsigned seed) {
  Bytes bytes(count);
  for (std::size_t i = 0; i < count; ++i) bytes[i] = (i * 37 + seed * 13) % 251;
  return bytes;
}
int run_cases(int argc, char** argv, std::span<const Case> cases) {
  if (argc == 2) {
    for (const auto& test : cases) {
      if (std::string_view{argv[1]} == "--list")
        std::cout << test.name << '\n';
      else if (std::string_view{argv[1]} == test.name) {
        test.run();
        return 0;
      }
    }
    return std::string_view{argv[1]} == "--list" ? 0 : 2;
  }
  unsigned failed = 0;
  // Each child starts with a pristine production stack; no reset/test hooks
  // are compiled into the kernel. Virtual time advances only at test requests.
  for (const auto& test : cases) {
    std::cout << "RUN: " << test.name << std::endl;
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
      alarm(20);
      test.run();
      std::exit(0);
    }
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::cout << (passed ? "PASS: " : "FAIL: ") << test.name << std::endl;
    failed += !passed;
  }
  std::cout << cases.size() - failed << '/' << cases.size()
            << " network scenarios passed\n";
  return failed != 0;
}
}  // namespace tcp_test

namespace mikos::arch {
u64 time_ticks() { return tcp_test::now; }
}  // namespace mikos::arch
namespace mikos {
void uart_put(char) {}
void uart_write(const char*, u32) {}
void write_text(const char*) {}
void write_u32(u32) {}
}  // namespace mikos
namespace mikos::drivers::net {
bool initialize() { return true; }
MacAddress mac_address() { return {{2, 0, 0, 0, 0, 2}}; }
bool receive(Frame& frame) {
  if (tcp_test::incoming.empty()) return false;
  tcp_test::receiving = std::move(tcp_test::incoming.front());
  tcp_test::incoming.pop_front();
  frame = {tcp_test::receiving.data(),
           static_cast<u32>(tcp_test::receiving.size())};
  return true;
}
bool transmit(const u8* frame, u32 size) {
  if (tcp_test::fail_transmits != 0) {
    --tcp_test::fail_transmits;
    return false;
  }
  tcp_test::record({frame, size});
  return true;
}
}  // namespace mikos::drivers::net
