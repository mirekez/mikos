#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mikos/arch.hpp>
#include <mikos/kernel.hpp>
#include <mikos/net/tcp.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace tcp_test {
using namespace mikos;
using namespace mikos::network;
using Bytes = std::vector<u8>;
inline constexpr u64 ticks_per_ms = 10'000;
inline constexpr Ipv4Address guest_ip{{10, 0, 2, 15}};
inline constexpr Ipv4Address peer_ip{{10, 0, 2, 2}};

void require(bool passed, std::string_view expression, int line);
#define REQUIRE(...) \
  ::tcp_test::require(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Segment {
  u32 sequence{}, acknowledgement{};
  u16 port{}, window{};
  u8 flags{};
  Bytes options, payload;
};

struct Session {
  SocketHandle handle{};
  u16 port{40000};
  u32 peer_next{1001}, server_next{};
};

// Packet construction and validation deliberately do not use parse_tcp(),
// TcpHeader, or the production checksum helpers: this is a wire-level oracle.
[[nodiscard]] u16 checksum(std::span<const u8> bytes);
[[nodiscard]] Bytes packet(u16 port, u8 flags, u32 sequence,
                           u32 acknowledgement, u16 window,
                           std::span<const u8> data = {},
                           std::span<const u8> options = {});

struct Harness {
  SocketHandle listener{};
  Harness();
  Session connect(u16 window = 65535, std::span<const u8> options = {},
                  u16 port = 40000, u32 sequence = 1000);
  void send(const Session& session, u8 flags, u32 sequence, u32 ack,
            u16 window = 65535, std::span<const u8> data = {},
            std::span<const u8> options = {});
  void ack(const Session& s, u32 ack, u16 window = 65535) {
    send(s, tcp_ack, s.peer_next, ack, window);
  }
  void receive(Bytes frame);
  void advance(u64 milliseconds);
  void ticks(u64 count);
  void idle(unsigned polls = 1);
  void clear();
  [[nodiscard]] std::vector<Segment> data() const;
  [[nodiscard]] Bytes read(const Session& s, std::size_t count);
  [[nodiscard]] ReadResult write(const Session& s, std::span<const u8> bytes) {
    return socket_write(s.handle, bytes.data(), bytes.size());
  }
};

extern u64 now;
extern std::deque<Bytes> incoming;
extern std::vector<Segment> outgoing;
extern unsigned fail_transmits;
Bytes pattern(std::size_t count, unsigned seed = 0);
struct Case {
  const char* name;
  void (*run)();
};
int run_cases(int argc, char** argv, std::span<const Case> cases);
}  // namespace tcp_test
