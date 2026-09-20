// Optional integration adapter: the production stack serves a real Linux TCP
// client through the existing Tribe TAP bridge. No simulated CPU is needed.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <drivers/net/net.hpp>
#include <iostream>
#include <mikos/arch.hpp>
#include <mikos/kernel.hpp>
#include <span>
#include <thread>

namespace {
int bridge_fd = -1;
std::array<mikos::u8, 2048> received;
const char* bridge_path;
const char* client_path;
constexpr mikos::Ipv4Address address{{192, 168, 76, 2}};
}  // namespace
namespace mikos::arch {
u64 time_ticks() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() /
         100;
}
}  // namespace mikos::arch
namespace mikos {
void uart_put(char) {}
void uart_write(const char*, u32) {}
void write_text(const char*) {}
void write_u32(u32) {}
}  // namespace mikos
namespace mikos::drivers::net {
bool initialize() {
  bridge_fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (bridge_fd < 0) return false;
  sockaddr_un local{}, remote{};
  local.sun_family = remote.sun_family = AF_UNIX;
  if (std::strlen(client_path) >= sizeof(local.sun_path) ||
      std::strlen(bridge_path) >= sizeof(remote.sun_path))
    return false;
  std::strcpy(local.sun_path, client_path);
  std::strcpy(remote.sun_path, bridge_path);
  if (::bind(bridge_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) !=
          0 ||
      ::connect(bridge_fd, reinterpret_cast<sockaddr*>(&remote),
                sizeof(remote)) != 0)
    return false;
  const u8 hello = 1;
  return ::send(bridge_fd, &hello, 1, 0) == 1;
}
MacAddress mac_address() { return {{2, 0, 0, 0, 0, 2}}; }
bool receive(Frame& frame) {
  const auto count = ::recv(bridge_fd, received.data(), received.size(), 0);
  if (count < 3 || received[0] != 2) return false;
  const u32 size = (u32{received[1]} << 8) | received[2];
  if (size + 3 != count) return false;
  frame = {received.data() + 3, size};
  return true;
}
bool transmit(const u8* data, u32 size) {
  std::array<u8, 2048> message{};
  if (size + 3 > message.size()) return false;
  message[0] = 2;
  message[1] = size >> 8;
  message[2] = size;
  std::copy_n(data, size, message.begin() + 3);
  return ::send(bridge_fd, message.data(), size + 3, 0) == size + 3;
}
}  // namespace mikos::drivers::net
int main(int argc, char** argv) {
  using namespace mikos;
  using namespace mikos::network;
  if (argc != 3) return 2;
  bridge_path = argv[1];
  client_path = argv[2];
  if (!initialize()) {
    std::perror("bridge initialization");
    return 1;
  }
  Ifreq32 request{};
  write_interface_name(request.name);
  write_sockaddr_ipv4(request.value.address, address);
  if (interface_ioctl(siocsifaddr, request) != InterfaceControlResult::success)
    return 1;
  const auto listener = socket_open(abi::socket::Type::stream);
  if (socket_bind(listener.handle, {address, 2223}) != SocketResult::success ||
      socket_listen(listener.handle, 4) != SocketResult::success)
    return 1;
  std::cout << "READY" << std::endl;
  SocketHandle active{};
  std::array<u8, 4096> buffer{};
  u32 remaining = 0, offset = 0, complete = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    mikos::network::poll();
    if (active == invalid_socket && complete < 3) {
      const auto accepted = socket_accept(listener.handle);
      if (accepted.result == SocketResult::success) active = accepted.handle;
    }
    if (active != invalid_socket) {
      if (remaining == 0) {
        const auto result = socket_read(active, buffer.data(), buffer.size());
        if (result.result == SocketResult::success) {
          remaining = result.size;
          offset = 0;
        } else if (result.result == SocketResult::end_of_file) {
          if (socket_close(active) != SocketResult::success) return 1;
          active = invalid_socket;
          ++complete;
        } else if (result.result != SocketResult::would_block)
          return 1;
      }
      if (remaining != 0) {
        const auto result =
            socket_write(active, buffer.data() + offset, remaining);
        if (result.result == SocketResult::success) {
          remaining -= result.size;
          offset += result.size;
        } else if (result.result != SocketResult::would_block)
          return 1;
      }
    }
    if (complete == 3) {
      // Let the peer's final ACK reach LAST-ACK before the test adapter exits.
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
      while (std::chrono::steady_clock::now() < until) {
        mikos::network::poll();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      ::close(bridge_fd);
      std::cout << "PASS: production TCP / Linux peer (three sessions)"
                << std::endl;
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  std::cerr << "FAIL: Linux peer deadline\n";
  return 1;
}
