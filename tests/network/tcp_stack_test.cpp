#include <limits>
#include <random>
#include <set>

#include "harness.hpp"

using namespace tcp_test;
namespace {
constexpr std::array<u8, 4> mss256{2, 4, 1, 0};
constexpr std::array<u8, 8> scaled{2, 4, 4, 0, 1, 3, 3, 4};

void transfer(unsigned profile) {
  Harness h;
  auto s = h.connect();
  const auto source = pattern(40'000, profile);
  Bytes delivered;
  u32 ack = s.server_next;
  for (unsigned round = 0; delivered.size() < source.size() && round < 2000;
       ++round) {
    const u16 window = profile == 0   ? 8192
                       : profile == 1 ? 37
                       : profile == 2 ? (round % 3 == 0 ? 0 : 511)
                       : profile == 3 ? (round % 4 == 0 ? 4096 : 0)
                                      : 1500;
    h.ack(s, ack, window);
    h.clear();
    const auto remaining = std::span{source}.subspan(delivered.size());
    const auto written = h.write(s, remaining);
    if (window == 0) {
      REQUIRE(written.size == 0 && written.result == SocketResult::would_block);
      REQUIRE(!socket_writable(s.handle));
      REQUIRE(h.data().empty());
      h.advance(50);
      continue;
    }
    REQUIRE(written.result == SocketResult::success);
    REQUIRE(written.size > 0 && written.size <= window);
    auto segments = h.data();
    REQUIRE(!segments.empty());
    Bytes flight;
    for (const auto& segment : segments) {
      REQUIRE(segment.sequence == ack + flight.size());
      REQUIRE(segment.payload.size() <=
              536);  // IPv4 default when MSS is absent.
      flight.insert(flight.end(), segment.payload.begin(),
                    segment.payload.end());
    }
    REQUIRE(flight.size() == written.size);
    REQUIRE(std::ranges::equal(flight, remaining.first(written.size)));
    if (profile == 4 && round % 7 == 0) {
      // Sparse loss: withhold ACK until the first retransmission, then ACK
      // the verified complete flight as a receiver retaining reordered data.
      h.clear();
      h.advance(1000);
      REQUIRE(h.data().size() == 1);
      REQUIRE(h.data().front().sequence == ack);
      REQUIRE(h.data().front().payload == segments.front().payload);
    } else if (profile == 1) {
      h.advance(250);
    }
    delivered.insert(delivered.end(), flight.begin(), flight.end());
    ack += written.size;
    h.ack(s, ack, window);
  }
  REQUIRE(delivered == source);
  h.clear();
  h.advance(60'000);
  REQUIRE(h.data().empty());
}
void fast() { transfer(0); }
void slow() { transfer(1); }
void intermittent() { transfer(2); }
void bursty() { transfer(3); }
void sparse_loss() { transfer(4); }

void zero_reopen() {
  Harness h;
  const auto s = h.connect(0);
  const auto bytes = pattern(50);
  REQUIRE(h.write(s, bytes).result == SocketResult::would_block);
  REQUIRE(h.data().empty() && !socket_writable(s.handle));
  h.ack(s, s.server_next, 17);
  REQUIRE(socket_writable(s.handle));
  REQUIRE(h.write(s, bytes).size == 17);
  REQUIRE(h.data().front().sequence == s.server_next);
  REQUIRE(h.write(s, bytes).result == SocketResult::would_block);
}
void partial_ack_and_shrink() {
  Harness h;
  const auto s = h.connect(100);
  const auto bytes = pattern(100);
  REQUIRE(h.write(s, bytes).size == 100);
  h.ack(s, s.server_next + 37, 5);
  REQUIRE(h.write(s, bytes).size == 0);
  h.clear();
  h.advance(1000);
  REQUIRE(h.data().size() == 1);
  REQUIRE(h.data().front().sequence == s.server_next + 37);
  REQUIRE(h.data().front().payload ==
          Bytes(bytes.begin() + 37, bytes.begin() + 42));
  h.ack(s, s.server_next + 100, 100);
  REQUIRE(h.write(s, bytes).size == 100);
}
void stale_and_future_acks() {
  Harness h;
  const auto s = h.connect(64);
  const auto bytes = pattern(64);
  REQUIRE(h.write(s, bytes).size == 64);
  h.ack(s, s.server_next + 65, 65535);  // Must not release storage/open window.
  REQUIRE(h.write(s, bytes).size == 0);
  h.ack(s, s.server_next + 64, 0);
  h.ack(s, s.server_next, 65535);  // Old ACK must not resurrect an old window.
  REQUIRE(h.write(s, bytes).size == 0);
  h.ack(s, s.server_next + 64, 9);
  REQUIRE(h.write(s, bytes).size == 9);
}
void receive_window() {
  Harness h;
  auto s = h.connect();
  const auto bytes = pattern(4096);
  for (unsigned offset = 0; offset < bytes.size(); offset += 1024)
    h.send(s, tcp_ack, s.peer_next + offset, s.server_next, 65535,
           std::span{bytes}.subspan(offset, 1024));
  REQUIRE(outgoing.back().window == 0);
  const auto last_ack = outgoing.back().acknowledgement;
  h.send(s, tcp_ack, last_ack, s.server_next, 65535, pattern(20));
  REQUIRE(outgoing.back().acknowledgement == last_ack);
  h.clear();
  REQUIRE(h.read(s, 100) == Bytes(bytes.begin(), bytes.begin() + 100));
  h.idle();
  REQUIRE(!outgoing.empty() && outgoing.back().window >= 100);
  REQUIRE(outgoing.back().acknowledgement == last_ack);
  REQUIRE(h.read(s, 4096) == Bytes(bytes.begin() + 100, bytes.end()));
}
void reordered_wraparound() {
  Harness h;
  auto s = h.connect(65535, {}, 40000, 0xfffffff0u);
  const auto bytes = pattern(64);
  h.send(s, tcp_ack, s.peer_next + 16, s.server_next, 65535,
         std::span{bytes}.subspan(16, 16));
  h.send(s, tcp_ack, s.peer_next + 32, s.server_next, 65535,
         std::span{bytes}.subspan(32));
  h.send(s, tcp_ack, s.peer_next + 16, s.server_next, 65535,
         std::span{bytes}.subspan(16, 16));
  h.send(s, tcp_ack, s.peer_next, s.server_next, 65535,
         std::span{bytes}.first(24));
  REQUIRE(outgoing.back().acknowledgement == u32(s.peer_next + bytes.size()));
  REQUIRE(h.read(s, 100) == bytes);
}
void receive_right_edge() {
  Harness h;
  const auto s = h.connect();
  h.send(s, tcp_ack, s.peer_next + 4095, s.server_next, 65535, pattern(10));
  const auto prefix = pattern(4095, 3);
  for (std::size_t offset = 0; offset < prefix.size();) {
    const auto count = std::min<std::size_t>(1000, prefix.size() - offset);
    h.send(s, tcp_ack, s.peer_next + offset, s.server_next, 65535,
           std::span{prefix}.subspan(offset, count));
    offset += count;
  }
  auto expected = prefix;
  expected.push_back(pattern(10)[0]);
  REQUIRE(h.read(s, 4096) == expected);
  REQUIRE(outgoing.back().acknowledgement == s.peer_next + 4096);
}

void mss_option() {
  Harness h;
  const auto s = h.connect(65535, mss256);
  const auto bytes = pattern(1000);
  REQUIRE(h.write(s, bytes).size == bytes.size());
  REQUIRE(h.data().size() == 4);
  for (const auto& segment : h.data()) REQUIRE(segment.payload.size() <= 256);
  constexpr std::array<u8, 4> midstream_mss{2, 4, 0, 1};
  h.send(s, tcp_ack, s.peer_next, s.server_next + 1000, 65535, {},
         midstream_mss);
  h.clear();
  REQUIRE(h.write(s, bytes).size == bytes.size());
  REQUIRE(h.data().size() == 4);
  for (const auto& segment : h.data()) REQUIRE(segment.payload.size() <= 256);
}
void window_scale() {
  Harness h;
  Session pending{};
  h.send(pending, tcp_syn, 1000, 0, 2, {}, scaled);
  REQUIRE(!outgoing.empty());
  const auto syn = outgoing.back();
  REQUIRE(
      std::ranges::search(syn.options, std::array<u8, 3>{3, 3, 0}).begin() !=
      syn.options.end());
  pending.server_next = syn.sequence + 1;
  h.ack(pending, pending.server_next, 2);
  const auto accepted = socket_accept(h.listener);
  REQUIRE(accepted.result == SocketResult::success);
  pending.handle = accepted.handle;
  h.clear();
  REQUIRE(h.write(pending, pattern(100)).size == 32);
}
void scale_without_negotiation() {
  Harness h;
  const auto s = h.connect(8);
  h.send(s, tcp_ack, s.peer_next, s.server_next, 8, {}, scaled);
  REQUIRE(h.write(s, pattern(100)).size == 8);
}
void malformed_options() {
  Harness h;
  Session pending{};
  const std::array<Bytes, 10> bad{{{2, 0, 0, 0},
                                   {2, 1, 0, 0},
                                   {2, 3, 1, 0},
                                   {2, 4, 0, 0},
                                   {3, 4, 1, 0},
                                   {8, 10, 0, 0},
                                   {30, 5, 0, 0},
                                   {5, 3, 0, 0},
                                   {2, 4, 1, 0, 2, 4, 2, 0},
                                   {3, 3, 1, 3, 3, 2, 0, 0}}};
  for (const auto& options : bad) {
    h.send(pending, tcp_syn, 1000, 0, 65535, {}, options);
    REQUIRE(outgoing.empty());
    REQUIRE(!socket_readable(h.listener));
  }
  const auto s = h.connect(65535, mss256);
  REQUIRE(h.write(s, pattern(300)).size == 300);
}
void optional_extensions() {
  Harness h;
  const std::array<u8, 20> options{1, 1, 4, 2, 8,  10, 0, 0, 0, 1,
                                   0, 0, 0, 0, 30, 4,  1, 2, 0, 0};
  Session p;
  h.send(p, tcp_syn, 1000, 0, 65535, {}, options);
  REQUIRE(!outgoing.empty());
  // Unsupported timestamps/SACK must not be advertised as negotiated.
  REQUIRE(outgoing.back().options == Bytes({2, 4, 0x05, 0xb4}));
  p.server_next = outgoing.back().sequence + 1;
  h.ack(p, p.server_next);
  REQUIRE(socket_accept(h.listener).result == SocketResult::success);
}
void scale_limit() {
  Harness h;
  auto options = scaled;
  options.back() = 255;
  const auto s = h.connect(2, options);
  REQUIRE(h.write(s, pattern(16'384)).size ==
          4096);  // 4*MSS congestion window, not an overflowing scale.
}

void retransmission_deadline() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(10)).size == 10);
  const auto original = h.data().front();
  h.clear();
  h.idle(100'000);
  REQUIRE(outgoing.empty());
  h.ticks(10'000'000 - 1);
  REQUIRE(outgoing.empty());
  h.ticks(1);
  REQUIRE(h.data().size() == 1 && h.data().front().payload == original.payload);
  h.clear();
  h.advance(1999);
  REQUIRE(outgoing.empty());
  h.advance(1);
  REQUIRE(h.data().size() == 1);
}
void ack_at_deadline() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(10)).size == 10);
  h.clear();
  now += 10'000'000;
  h.ack(s, s.server_next + 10);
  REQUIRE(h.data().empty());
  h.advance(120'000);
  REQUIRE(h.data().empty());
  REQUIRE(socket_writable(s.handle));
}
void data_timeout() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(100)).size == 100);
  h.clear();
  h.advance(119'999);
  u8 byte{};
  REQUIRE(socket_read(s.handle, &byte, 1).result == SocketResult::would_block);
  h.advance(1);
  REQUIRE(socket_read(s.handle, &byte, 1).result == SocketResult::timed_out);
  REQUIRE(!socket_writable(s.handle));
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  const auto fresh = h.connect(65535, {}, 40001);
  REQUIRE(h.write(fresh, pattern(100)).size == 100);
}
void syn_expiry() {
  Harness h;
  Session p;
  for (unsigned i = 0; i < 20; ++i) {
    p.port = 41000 + i;
    h.send(p, tcp_syn, 1000, 0);
  }
  REQUIRE(outgoing.size() == listen_backlog_capacity);
  h.clear();
  h.advance(999);
  REQUIRE(outgoing.empty());
  h.advance(1);
  h.idle(10);
  REQUIRE(outgoing.size() == listen_backlog_capacity);
  h.advance(120'000);
  h.clear();
  const auto s = h.connect(65535, {}, 45000);
  REQUIRE(s.handle != invalid_socket);
}
void zero_window_persist() {
  Harness h;
  const auto s = h.connect(0);
  REQUIRE(h.write(s, pattern(10)).size == 0);
  h.clear();
  h.advance(999);
  REQUIRE(outgoing.empty());
  h.advance(1);
  REQUIRE(outgoing.size() == 1);
  REQUIRE(outgoing.back().payload.size() <= 1);
  // A responsive peer may stay at zero indefinitely without being reset.
  for (unsigned i = 0; i < 20; ++i) {
    h.ack(s, s.server_next, 0);
    h.advance(30'000);
  }
  h.ack(s, s.server_next, 10);
  REQUIRE(h.write(s, pattern(10)).size == 10);
}
void silent_persist_timeout() {
  Harness h;
  const auto s = h.connect(0);
  REQUIRE(h.write(s, pattern(10)).size == 0);
  h.advance(120'000);
  u8 output{};
  REQUIRE(socket_read(s.handle, &output, 1).result !=
          SocketResult::would_block);
  REQUIRE(!socket_writable(s.handle));
}
void idle_connection() {
  Harness h;
  const auto s = h.connect();
  h.advance(24ull * 60 * 60 * 1000);
  REQUIRE(outgoing.empty());
  REQUIRE(h.write(s, pattern(20)).size == 20);
}
void clock_wrap() {
  now = std::numeric_limits<u64>::max() - 5'000'000;
  retransmission_deadline();
}
void driver_busy() {
  Harness h;
  const auto s = h.connect();
  fail_transmits = 1;
  REQUIRE(h.write(s, pattern(10)).size == 0);
  REQUIRE(h.write(s, pattern(10)).size == 10);
  REQUIRE(h.data().front().sequence == s.server_next);
}

void transmit_flood() {
  Harness h;
  const auto s = h.connect();
  for (unsigned cycle = 0; cycle < 20; ++cycle) {
    for (unsigned i = 0; i < transmit_capacity; ++i)
      REQUIRE(h.write(s, pattern(1)).size == 1);
    REQUIRE(h.write(s, pattern(1)).size == 0);
    REQUIRE(!socket_writable(s.handle));
    h.ack(s, s.server_next + (cycle + 1) * transmit_capacity);
    REQUIRE(socket_writable(s.handle));
  }
}
void rst_cleanup() {
  Harness h;
  for (unsigned cycle = 0; cycle < 40; ++cycle) {
    auto s = h.connect(65535, {}, 40000 + cycle);
    for (unsigned i = 0; i < transmit_capacity; ++i)
      REQUIRE(h.write(s, pattern(1)).size == 1);
    for (unsigned i = 1; i <= reassembly_capacity; ++i)
      h.send(s, tcp_ack, s.peer_next + i, s.server_next, 65535, pattern(1));
    h.send(s, tcp_rst, s.peer_next, 0);
    REQUIRE(socket_close(s.handle) == SocketResult::success);
    h.clear();
    h.advance(5000);
    REQUIRE(outgoing.empty());
  }
}
void reject_far_sequence_flood() {
  Harness h;
  const auto s = h.connect();
  for (unsigned i = 0; i < 1000; ++i)
    h.send(s, tcp_ack, s.peer_next + 100'000 + i, s.server_next, 65535,
           pattern(1));
  const auto bytes = pattern(32);
  h.send(s, tcp_ack, s.peer_next + 1, s.server_next, 65535,
         std::span{bytes}.subspan(1));
  h.send(s, tcp_ack, s.peer_next, s.server_next, 65535,
         std::span{bytes}.first(1));
  REQUIRE(h.read(s, 100) == bytes);
}
void spoofed_reset() {
  Harness h;
  const auto s = h.connect();
  h.send(s, tcp_rst, s.peer_next + 100'000, 0);
  REQUIRE(socket_writable(s.handle));
  h.send(s, tcp_rst, s.peer_next + 1,
         0);  // In-window, but not exact: challenge ACK.
  REQUIRE(socket_writable(s.handle));
  h.send(s, tcp_rst, s.peer_next, 0);
  REQUIRE(!socket_writable(s.handle));
}
void syn_flags_and_invalid_ack() {
  Harness h;
  Session p;
  h.send(p, tcp_syn | tcp_rst, 1000, 0);
  REQUIRE(outgoing.empty());
  h.send(p, tcp_syn | tcp_fin, 1000, 0);
  REQUIRE(outgoing.empty());
  h.send(p, tcp_syn, 1000, 0);
  REQUIRE(!outgoing.empty());
  p.server_next = outgoing.back().sequence + 1;
  h.send(p, tcp_ack, 7000, p.server_next);
  REQUIRE(socket_accept(h.listener).result == SocketResult::would_block);
  h.ack(p, p.server_next);
  REQUIRE(socket_accept(h.listener).result == SocketResult::success);
}
void duplicate_fin() {
  Harness h;
  const auto s = h.connect();
  h.send(s, tcp_ack | tcp_fin, s.peer_next, s.server_next);
  const auto ack = outgoing.back().acknowledgement;
  h.send(s, tcp_ack | tcp_fin, s.peer_next, s.server_next);
  REQUIRE(outgoing.back().acknowledgement == ack);
  h.send(s, tcp_ack, ack, s.server_next, 65535, pattern(5));
  u8 byte{};
  REQUIRE(socket_read(s.handle, &byte, 1).result == SocketResult::end_of_file);
  REQUIRE(h.write(s, pattern(5)).size ==
          5);  // Half-close still allows responses.
}
void fin_retransmission() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_shutdown(s.handle, 1) == SocketResult::success);
  REQUIRE(outgoing.back().flags == (tcp_fin | tcp_ack));
  const auto sequence = outgoing.back().sequence;
  h.clear();
  h.advance(1000);
  REQUIRE(outgoing.size() == 1 && outgoing.back().sequence == sequence);
  REQUIRE(outgoing.back().flags == (tcp_fin | tcp_ack));
  h.ack(s, sequence + 1);
  h.clear();
  h.advance(10'000);
  REQUIRE(outgoing.empty());
}
void close_retains_data() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(100)).size == 100);
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  h.clear();
  h.advance(1000);
  REQUIRE(h.data().size() == 1 && h.data().front().payload == pattern(100));
  h.ack(s, s.server_next + 100);
  REQUIRE(outgoing.back().flags == (tcp_fin | tcp_ack));
  REQUIRE(outgoing.back().sequence == s.server_next + 100);
}
void fin_wait_timeout() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  h.ack(s, s.server_next + 1);
  h.advance(120'000);
  REQUIRE(socket_slot(s.handle) == nullptr);
}
void time_wait() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  h.send(s, tcp_fin | tcp_ack, s.peer_next, s.server_next + 1);
  REQUIRE(socket_slot(s.handle) != nullptr);
  h.advance(119'999);
  REQUIRE(socket_slot(s.handle) != nullptr);
  h.advance(1);
  REQUIRE(socket_slot(s.handle) == nullptr);
}
void two_flows() {
  Harness h;
  const auto a = h.connect(32);
  const auto b = h.connect(32, {}, 40001);
  REQUIRE(h.write(a, pattern(32, 1)).size == 32);
  REQUIRE(h.write(b, pattern(32, 2)).size == 32);
  h.ack(a, a.server_next + 32, 0);
  h.clear();
  h.advance(1000);
  h.idle(4);
  REQUIRE(h.data().size() == 1 && h.data().front().port == b.port);
  REQUIRE(h.data().front().payload == pattern(32, 2));
}

void time_wait_reset() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  h.send(s, tcp_fin | tcp_ack, s.peer_next, s.server_next + 1);
  h.advance(60'000);
  h.clear();
  h.send(s, tcp_rst, s.peer_next + 1, 0);
  REQUIRE(socket_slot(s.handle) != nullptr);
  REQUIRE(socket_slot(s.handle)->state == SocketState::time_wait);
  REQUIRE(outgoing.empty());
  h.advance(59'999);
  REQUIRE(socket_slot(s.handle) != nullptr);
  h.advance(1);
  REQUIRE(socket_slot(s.handle) == nullptr);
}

void closed_port() {
  Harness h;
  REQUIRE(socket_close(h.listener) == SocketResult::success);
  const Session s;
  h.send(s, tcp_syn, 1000, 0);
  REQUIRE(outgoing.size() == 1);
  REQUIRE(outgoing.back().flags == (tcp_rst | tcp_ack));
  REQUIRE(outgoing.back().sequence == 0);
  REQUIRE(outgoing.back().acknowledgement == 1001);
  REQUIRE(outgoing.back().window == 0);
  h.clear();
  h.send(s, tcp_ack, 1001, 5000);
  REQUIRE(outgoing.size() == 1);
  REQUIRE(outgoing.back().flags == tcp_rst);
  REQUIRE(outgoing.back().sequence == 5000);
  h.clear();
  h.send(s, tcp_rst, 1001, 0);
  REQUIRE(outgoing.empty());
  for (unsigned i = 0; i < 1000; ++i) h.send(s, tcp_syn, 1000, 0);
  REQUIRE(outgoing.size() == 98);  // Shared 100/s control-response budget.
  h.advance(1000);
  h.clear();
  h.send(s, tcp_syn, 1000, 0);
  REQUIRE(outgoing.size() == 1);
}
void adaptive_rto_and_karn() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(20)).size == 20);
  h.advance(800);
  h.ack(s, s.server_next + 20);  // First RTT=800ms -> RTO=2400ms.
  REQUIRE(h.write(s, pattern(20)).size == 20);
  h.clear();
  h.advance(2399);
  REQUIRE(outgoing.empty());
  h.advance(1);
  REQUIRE(h.data().size() == 1);
  h.advance(200);
  h.ack(s, s.server_next + 40);  // Ambiguous retransmitted sample ignored.
  REQUIRE(h.write(s, pattern(20)).size == 20);
  h.clear();
  h.advance(2399);
  REQUIRE(outgoing.empty());
  h.advance(1);
  REQUIRE(h.data().size() == 1);
}
void backoff_cap() {
  Harness h;
  const auto s = h.connect(0);
  REQUIRE(h.write(s, pattern(1)).size == 0);
  for (const u64 delay : {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000}) {
    h.ack(s, s.server_next, 0);
    h.clear();
    h.advance(delay - 1);
    REQUIRE(outgoing.empty());
    h.advance(1);
    REQUIRE(outgoing.size() == 1);
  }
}
void outstanding_zero_window() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(20)).size == 20);
  for (unsigned i = 0; i < 20; ++i) {
    h.ack(s, s.server_next, 0);
    h.advance(30'000);
  }
  h.ack(s, s.server_next + 20, 20);
  REQUIRE(h.write(s, pattern(20)).size == 20);
}
void fin_timeout() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  h.advance(119'999);
  REQUIRE(socket_slot(s.handle) != nullptr);
  h.advance(1);
  REQUIRE(socket_slot(s.handle) == nullptr);
}
void last_ack_cleanup() {
  Harness h;
  const auto s = h.connect();
  h.send(s, tcp_fin | tcp_ack, s.peer_next, s.server_next);
  REQUIRE(socket_shutdown(s.handle, 1) == SocketResult::success);
  h.send(s, tcp_ack, s.peer_next + 1, s.server_next + 1);
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  REQUIRE(socket_slot(s.handle) == nullptr);
}
void time_wait_restart() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  h.send(s, tcp_fin | tcp_ack, s.peer_next, s.server_next + 1);
  h.advance(119'000);
  h.send(s, tcp_fin | tcp_ack, s.peer_next, s.server_next + 1);
  h.advance(1000);
  REQUIRE(socket_slot(s.handle) != nullptr);
  h.advance(119'000);
  REQUIRE(socket_slot(s.handle) == nullptr);
}
void attached_time_wait_cleanup() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(socket_shutdown(s.handle, 1) == SocketResult::success);
  h.send(s, tcp_fin | tcp_ack, s.peer_next, s.server_next + 1);
  h.advance(120'000);
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  REQUIRE(socket_slot(s.handle) == nullptr);
}
void syn_busy_timeout() {
  Harness h;
  Session s;
  fail_transmits = 1'000'000;
  h.send(s, tcp_syn, 1000, 0);
  h.advance(120'000);
  fail_transmits = 0;
  REQUIRE(h.connect().handle != invalid_socket);
}
void rto_busy_timeout() {
  Harness h;
  const auto s = h.connect();
  REQUIRE(h.write(s, pattern(20)).size == 20);
  fail_transmits = 1'000'000;
  h.advance(120'000);
  REQUIRE(!socket_writable(s.handle));
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  fail_transmits = 0;
  REQUIRE(h.connect(65535, {}, 40001).handle != invalid_socket);
}
void congestion_and_fast_retransmit() {
  Harness h;
  const auto s = h.connect(65535, mss256);
  const auto bytes = pattern(4096);
  REQUIRE(h.write(s, bytes).size ==
          1024);  // Initial congestion window = 4*MSS.
  REQUIRE(h.write(s, bytes).size == 0);
  h.clear();
  h.ack(s, s.server_next);
  h.ack(s, s.server_next);
  REQUIRE(h.data().empty());
  h.ack(s, s.server_next);
  REQUIRE(h.data().size() == 1 && h.data().front().sequence == s.server_next);
  REQUIRE(h.data().front().payload ==
          Bytes(bytes.begin(), bytes.begin() + 256));
  h.ack(s, s.server_next + 1024);
  h.clear();
  REQUIRE(h.write(s, bytes).size == 512);  // Flight/2 after fast recovery.
  h.clear();
  h.advance(1000);
  REQUIRE(h.data().size() == 1);
  h.ack(s, s.server_next + 1536);
  h.clear();
  REQUIRE(h.write(s, bytes).size <= 512);  // RTO reset then slow start.
}
void reassembly_exhaustion() {
  Harness h;
  auto s = h.connect();
  for (unsigned cycle = 0; cycle < 10; ++cycle) {
    const auto bytes = pattern(40, cycle);
    for (unsigned i = 1; i <= 20; ++i)
      h.send(s, tcp_ack, s.peer_next + i, s.server_next, 65535,
             std::span{bytes}.subspan(i, 1));
    h.send(s, tcp_ack, s.peer_next, s.server_next, 65535,
           std::span{bytes}.first(1));
    REQUIRE(h.read(s, 40) == Bytes(bytes.begin(), bytes.begin() + 17));
    h.send(s, tcp_ack, s.peer_next + 17, s.server_next, 65535,
           std::span{bytes}.subspan(17));
    REQUIRE(h.read(s, 40) == Bytes(bytes.begin() + 17, bytes.end()));
    s.peer_next += bytes.size();
  }
}
void listener_clearance() {
  Harness h;
  Session p;
  for (unsigned cycle = 0; cycle < 20; ++cycle) {
    for (unsigned i = 0; i < listen_backlog_capacity; ++i) {
      p.port = 40000 + i;
      h.send(p, tcp_syn, 1000, 0);
    }
    REQUIRE(socket_close(h.listener) == SocketResult::success);
    const auto opened = socket_open(abi::socket::Type::stream);
    REQUIRE(opened.result == SocketResult::success);
    h.listener = opened.handle;
    REQUIRE(socket_bind(h.listener, {guest_ip, 22}) == SocketResult::success);
    REQUIRE(socket_listen(h.listener, 4) == SocketResult::success);
    h.clear();
    h.advance(1000);
    REQUIRE(outgoing.empty());
  }
  REQUIRE(h.connect().handle != invalid_socket);
}
void reset_challenge_limit() {
  Harness h;
  const auto s = h.connect();
  for (unsigned i = 0; i < 1000; ++i) h.send(s, tcp_rst, s.peer_next + 1, 0);
  REQUIRE(outgoing.size() <= 100);
  REQUIRE(socket_writable(s.handle));
}
void seeded_loss_reordering() {
  Harness h;
  for (unsigned seed = 1; seed <= 12; ++seed) {
    std::mt19937 rng(seed);
    const auto s = h.connect(65535, mss256, 42000 + seed);
    const auto source = pattern(64'000, seed);
    std::vector<bool> seen(source.size());
    std::set<u32> transmissions;
    std::size_t written = 0, contiguous = 0;
    for (unsigned step = 0; step < 20'000 && contiguous < source.size();
         ++step) {
      if (written < source.size())
        written += h.write(s, std::span{source}.subspan(written)).size;
      auto segments = h.data();
      h.clear();
      std::shuffle(segments.begin(), segments.end(), rng);
      for (const auto& segment : segments) {
        const bool first_send = transmissions.insert(segment.sequence).second;
        if (first_send && rng() % 7 == 0) continue;
        const u32 offset = segment.sequence - s.server_next;
        REQUIRE(offset + segment.payload.size() <= written);
        REQUIRE(segment.payload.size() <= 256);
        for (std::size_t i = 0; i < segment.payload.size(); ++i) {
          REQUIRE(segment.payload[i] == source[offset + i]);
          seen[offset + i] = true;
        }
        while (contiguous < source.size() && seen[contiguous]) ++contiguous;
        h.ack(s, s.server_next + contiguous);
      }
      h.advance(10);
    }
    REQUIRE(contiguous == source.size() && written == source.size());
    h.send(s, tcp_rst, s.peer_next, 0);
    REQUIRE(socket_close(s.handle) == SocketResult::success);
    h.clear();
  }
}

void full_socket_table() {
  Harness h;
  std::vector<Session> sessions;
  for (unsigned i = 0; i < socket_capacity - 1; ++i)
    sessions.push_back(h.connect(65535, {}, 44000 + i));
  Session extra;
  extra.port = 45000;
  h.send(extra, tcp_syn, 1000, 0);
  REQUIRE(outgoing.empty());
  REQUIRE(h.write(sessions.front(), pattern(8)).size == 8);
  for (const auto& s : sessions) {
    h.send(s, tcp_rst, s.peer_next, 0);
    REQUIRE(socket_close(s.handle) == SocketResult::success);
  }
  h.clear();
  REQUIRE(h.connect(65535, {}, 45000).handle != invalid_socket);
}
void completed_handshake_reset_flood() {
  Harness h;
  Session p;
  for (unsigned i = 0; i < 100; ++i) {
    p.port = 44000 + i;
    h.send(p, tcp_syn, 1000, 0);
    REQUIRE(!outgoing.empty() && outgoing.back().flags == (tcp_syn | tcp_ack));
    p.server_next = outgoing.back().sequence + 1;
    h.ack(p, p.server_next);
    h.send(p, tcp_rst, p.peer_next, 0);
    REQUIRE(socket_accept(h.listener).result == SocketResult::would_block);
    h.clear();
  }
  REQUIRE(h.connect().handle != invalid_socket);
}
void checksum_flood() {
  Harness h;
  const auto s = h.connect();
  std::mt19937 rng(8128);
  for (unsigned i = 0; i < 10'000; ++i) {
    auto frame =
        packet(s.port, tcp_ack, s.peer_next, s.server_next, 65535, pattern(32));
    // Changing any one TCP bit without updating checksum must be rejected.
    frame[34 + rng() % (frame.size() - 34)] ^= u8{1} << (rng() % 8);
    h.receive(std::move(frame));
  }
  REQUIRE(outgoing.empty());
  REQUIRE(h.write(s, pattern(32)).size == 32);
}
void duplicate_receive_flood() {
  Harness h;
  const auto s = h.connect();
  const auto bytes = pattern(100);
  for (unsigned i = 0; i < 1000; ++i)
    h.send(s, tcp_ack, s.peer_next, s.server_next, 65535, bytes);
  REQUIRE(h.read(s, 4096) == bytes);
  u8 byte{};
  REQUIRE(socket_read(s.handle, &byte, 1).result == SocketResult::would_block);
}
void two_due_flows() {
  Harness h;
  const auto a = h.connect();
  const auto b = h.connect(65535, {}, 40001);
  REQUIRE(h.write(a, pattern(20, 1)).size == 20);
  REQUIRE(h.write(b, pattern(20, 2)).size == 20);
  h.clear();
  h.advance(1000);
  h.idle();
  REQUIRE(h.data().size() == 2);
  REQUIRE(h.data()[0].port != h.data()[1].port);
}
void option_mss_bounds() {
  Harness h;
  const std::array<u8, 4> tiny{2, 4, 0, 1}, huge{2, 4, 255, 255};
  auto s = h.connect(65535, tiny);
  REQUIRE(h.write(s, pattern(20)).size == 4);
  for (const auto& segment : h.data()) REQUIRE(segment.payload.size() == 1);
  h.send(s, tcp_rst, s.peer_next, 0);
  REQUIRE(socket_close(s.handle) == SocketResult::success);
  s = h.connect(65535, huge, 40001);
  REQUIRE(h.write(s, pattern(10'000)).size == 4096);
  for (const auto& segment : h.data()) REQUIRE(segment.payload.size() <= 1024);
}
void stale_window_sequence() {
  Harness h;
  auto s = h.connect(100);
  h.send(s, tcp_ack, s.peer_next, s.server_next, 10, pattern(1));
  ++s.peer_next;
  h.ack(s, s.server_next, 10);
  h.send(s, tcp_ack, s.peer_next - 1, s.server_next, 65535);
  REQUIRE(h.write(s, pattern(100)).size == 10);
}
void duplicate_syn_deadline() {
  Harness h;
  Session p;
  h.send(p, tcp_syn, 1000, 0);
  const auto sequence = outgoing.back().sequence;
  for (unsigned i = 0; i < 119; ++i) {
    h.advance(1000);
    h.send(p, tcp_syn, 1000, 0);
    REQUIRE(outgoing.back().sequence == sequence);
  }
  h.advance(1000);
  h.clear();
  h.send(p, tcp_syn, 1000, 0);
  REQUIRE(outgoing.back().sequence != sequence);
}

}  // namespace

int main(int argc, char** argv) {
  const std::array cases{
      Case{"window/fast", fast},
      Case{"window/slow", slow},
      Case{"window/intermittent", intermittent},
      Case{"window/bursty", bursty},
      Case{"window/sparse_loss", sparse_loss},
      Case{"window/zero_reopen", zero_reopen},
      Case{"window/partial_ack_shrink", partial_ack_and_shrink},
      Case{"window/stale_future_ack", stale_and_future_acks},
      Case{"window/receive_reopen", receive_window},
      Case{"window/reorder_wrap", reordered_wraparound},
      Case{"window/right_edge", receive_right_edge},
      Case{"options/mss", mss_option},
      Case{"options/scale", window_scale},
      Case{"options/no_negotiation", scale_without_negotiation},
      Case{"options/malformed", malformed_options},
      Case{"options/extensions", optional_extensions},
      Case{"options/scale_limit", scale_limit},
      Case{"timeout/rto_backoff", retransmission_deadline},
      Case{"timeout/ack_at_deadline", ack_at_deadline},
      Case{"timeout/data", data_timeout},
      Case{"timeout/syn", syn_expiry},
      Case{"timeout/persist", zero_window_persist},
      Case{"timeout/silent_persist", silent_persist_timeout},
      Case{"timeout/idle", idle_connection},
      Case{"timeout/clock_wrap", clock_wrap},
      Case{"overflow/nic_busy", driver_busy},
      Case{"overflow/transmit", transmit_flood},
      Case{"overflow/rst_cleanup", rst_cleanup},
      Case{"overflow/out_of_window", reject_far_sequence_flood},
      Case{"control/reset_validation", spoofed_reset},
      Case{"control/handshake_validation", syn_flags_and_invalid_ack},
      Case{"control/duplicate_fin", duplicate_fin},
      Case{"control/fin_retry", fin_retransmission},
      Case{"control/close_retains_data", close_retains_data},
      Case{"control/fin_wait_timeout", fin_wait_timeout},
      Case{"control/time_wait", time_wait},
      Case{"control/time_wait_reset", time_wait_reset},
      Case{"control/closed_port", closed_port},
      Case{"control/two_flows", two_flows},
      Case{"timeout/adaptive_karn", adaptive_rto_and_karn},
      Case{"timeout/backoff_cap", backoff_cap},
      Case{"timeout/outstanding_persist", outstanding_zero_window},
      Case{"timeout/fin", fin_timeout},
      Case{"timeout/syn_nic_busy", syn_busy_timeout},
      Case{"timeout/rto_nic_busy", rto_busy_timeout},
      Case{"control/last_ack", last_ack_cleanup},
      Case{"control/time_wait_restart", time_wait_restart},
      Case{"control/attached_time_wait", attached_time_wait_cleanup},
      Case{"control/congestion_fast_retransmit",
           congestion_and_fast_retransmit},
      Case{"overflow/reassembly", reassembly_exhaustion},
      Case{"overflow/listener_cleanup", listener_clearance},
      Case{"overflow/challenge_rate", reset_challenge_limit},
      Case{"stress/seeded_loss_reorder", seeded_loss_reordering},
      Case{"overflow/socket_table", full_socket_table},
      Case{"overflow/completed_rst", completed_handshake_reset_flood},
      Case{"overflow/checksum_flood", checksum_flood},
      Case{"overflow/duplicate_data", duplicate_receive_flood},
      Case{"control/fair_retransmission", two_due_flows},
      Case{"options/mss_bounds", option_mss_bounds},
      Case{"window/stale_sequence", stale_window_sequence},
      Case{"timeout/duplicate_syn", duplicate_syn_deadline},
  };
  return run_cases(argc, argv, cases);
}
