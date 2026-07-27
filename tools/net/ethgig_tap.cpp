#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/if.h>
#include <linux/if_tun.h>

// Adapted from cpphdl/tribe/linux/net/ethgig_tap.cpp.  In addition to the
// Tribe simulator's three-byte framing, this copy understands QEMU's raw
// Unix-datagram netdev protocol and configures the host side of the link.

namespace {

volatile sig_atomic_t stop_requested = 0;

void signal_handler(int) { stop_requested = 1; }

struct Options {
  std::string tap = "tap-mikos";
  std::string socket = "/tmp/mikos-tap.sock";
  std::string peer;
  std::string address = "192.168.76.1";
  unsigned prefix = 24;
};

void usage(const char* program) {
  std::cerr
      << "usage: " << program
      << " [--tap NAME] [--socket PATH] [--peer QEMU_SOCKET]\n"
         "       [--address IPV4/PREFIX]\n\n"
         "Without --peer the socket uses Tribe's framed protocol. With --peer\n"
         "it relays the raw Ethernet datagrams used by QEMU -netdev dgram.\n";
}

bool split_address(const char* text, std::string& address, unsigned& prefix) {
  const std::string value(text);
  const auto slash = value.find('/');
  address = value.substr(0, slash);
  prefix = 24;
  if (slash != std::string::npos) {
    const std::string bits = value.substr(slash + 1);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(bits.c_str(), &end, 10);
    if (bits.empty() || *end != '\0' || parsed > 32) {
      return false;
    }
    prefix = static_cast<unsigned>(parsed);
  }
  in_addr parsed{};
  return inet_pton(AF_INET, address.c_str(), &parsed) == 1;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
      options.tap = argv[++i];
    } else if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      options.socket = argv[++i];
    } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
      options.peer = argv[++i];
    } else if (std::strcmp(argv[i], "--address") == 0 && i + 1 < argc) {
      if (!split_address(argv[++i], options.address, options.prefix)) {
        std::cerr << "invalid IPv4 prefix: " << argv[i] << '\n';
        return false;
      }
    } else if (std::strcmp(argv[i], "-h") == 0 ||
               std::strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      std::exit(0);
    } else {
      usage(argv[0]);
      return false;
    }
  }
  return !options.tap.empty() && !options.socket.empty();
}

bool make_unix_address(const std::string& path, sockaddr_un& address) {
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    std::cerr << "invalid Unix socket path: " << path << '\n';
    return false;
  }
  address = {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return true;
}

int open_socket(const std::string& path) {
  sockaddr_un address{};
  if (!make_unix_address(path, address)) {
    return -1;
  }
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::cerr << "socket: " << std::strerror(errno) << '\n';
    return -1;
  }
  unlink(path.c_str());
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "bind " << path << ": " << std::strerror(errno) << '\n';
    close(fd);
    return -1;
  }
  return fd;
}

bool set_interface_address(const Options& options) {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::cerr << "IPv4 control socket: " << std::strerror(errno) << '\n';
    return false;
  }

  ifreq request{};
  std::strncpy(request.ifr_name, options.tap.c_str(), IFNAMSIZ - 1);
  auto* address = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
  address->sin_family = AF_INET;
  inet_pton(AF_INET, options.address.c_str(), &address->sin_addr);
  if (ioctl(fd, SIOCSIFADDR, &request) != 0) {
    std::cerr << "SIOCSIFADDR " << options.tap << ": "
              << std::strerror(errno) << '\n';
    close(fd);
    return false;
  }

  request = {};
  std::strncpy(request.ifr_name, options.tap.c_str(), IFNAMSIZ - 1);
  auto* netmask = reinterpret_cast<sockaddr_in*>(&request.ifr_netmask);
  netmask->sin_family = AF_INET;
  const uint32_t mask = options.prefix == 0
                            ? 0
                            : 0xffffffffu << (32 - options.prefix);
  netmask->sin_addr.s_addr = htonl(mask);
  if (ioctl(fd, SIOCSIFNETMASK, &request) != 0) {
    std::cerr << "SIOCSIFNETMASK " << options.tap << ": "
              << std::strerror(errno) << '\n';
    close(fd);
    return false;
  }

  request = {};
  std::strncpy(request.ifr_name, options.tap.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFFLAGS, &request) != 0) {
    std::cerr << "SIOCGIFFLAGS " << options.tap << ": "
              << std::strerror(errno) << '\n';
    close(fd);
    return false;
  }
  request.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_PROMISC;
  const bool ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0;
  if (!ok) {
    std::cerr << "SIOCSIFFLAGS " << options.tap << ": "
              << std::strerror(errno) << '\n';
  }
  close(fd);
  return ok;
}

int open_tap(const Options& options) {
  const int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    std::cerr << "open /dev/net/tun: " << std::strerror(errno) << '\n';
    return -1;
  }
  ifreq request{};
  request.ifr_flags = IFF_TAP | IFF_NO_PI;
  std::strncpy(request.ifr_name, options.tap.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, TUNSETIFF, &request) != 0) {
    std::cerr << "TUNSETIFF " << options.tap << ": "
              << std::strerror(errno) << '\n';
    close(fd);
    return -1;
  }
  if (!set_interface_address(options)) {
    close(fd);
    return -1;
  }
  std::cout << "TAP " << options.tap << " host address " << options.address
            << '/' << options.prefix << '\n';
  return fd;
}

bool transient_error() { return errno == EAGAIN || errno == EWOULDBLOCK; }

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 2;
  }
  const int socket_fd = open_socket(options.socket);
  if (socket_fd < 0) {
    return 1;
  }
  const int tap_fd = open_tap(options);
  if (tap_fd < 0) {
    close(socket_fd);
    unlink(options.socket.c_str());
    return 1;
  }

  sockaddr_un peer{};
  socklen_t peer_size = 0;
  bool have_peer = false;
  const bool qemu_mode = !options.peer.empty();
  if (qemu_mode) {
    if (!make_unix_address(options.peer, peer)) {
      close(tap_fd);
      close(socket_fd);
      unlink(options.socket.c_str());
      return 2;
    }
    peer_size = sizeof(peer);
    have_peer = true;
    std::cout << "QEMU datagram peer " << options.peer << '\n';
  }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  uint64_t host_to_guest = 0;
  uint64_t guest_to_host = 0;

  while (!stop_requested) {
    pollfd descriptors[2]{{tap_fd, POLLIN, 0}, {socket_fd, POLLIN, 0}};
    const int ready = poll(descriptors, 2, 1000);
    if (ready < 0 && errno != EINTR) {
      std::cerr << "poll: " << std::strerror(errno) << '\n';
      break;
    }

    if ((descriptors[1].revents & POLLIN) != 0) {
      for (;;) {
        std::array<uint8_t, 4099> message{};
        sockaddr_un sender{};
        socklen_t sender_size = sizeof(sender);
        const ssize_t received = recvfrom(
            socket_fd, message.data(), message.size(), MSG_DONTWAIT,
            reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (received < 0) {
          if (transient_error()) break;
          std::cerr << "recvfrom: " << std::strerror(errno) << '\n';
          break;
        }

        const uint8_t* frame = message.data();
        size_t frame_size = static_cast<size_t>(received);
        if (!qemu_mode) {
          if (received == 1 && message[0] == 1) {
            peer = sender;
            peer_size = sender_size;
            have_peer = true;
            continue;
          }
          if (received < 3 || message[0] != 2) continue;
          frame_size = (static_cast<size_t>(message[1]) << 8) | message[2];
          if (frame_size > static_cast<size_t>(received - 3)) continue;
          frame += 3;
        }
        if (write(tap_fd, frame, frame_size) ==
            static_cast<ssize_t>(frame_size)) {
          ++guest_to_host;
        }
      }
    }

    if ((descriptors[0].revents & POLLIN) != 0) {
      for (;;) {
        std::array<uint8_t, 2051> message{};
        uint8_t* frame = message.data() + (qemu_mode ? 0 : 3);
        const ssize_t received = read(tap_fd, frame, 2048);
        if (received < 0) {
          if (transient_error()) break;
          std::cerr << "read TAP: " << std::strerror(errno) << '\n';
          break;
        }
        if (!have_peer) continue;
        size_t message_size = static_cast<size_t>(received);
        if (!qemu_mode) {
          message[0] = 2;
          message[1] = static_cast<uint8_t>(received >> 8);
          message[2] = static_cast<uint8_t>(received);
          message_size += 3;
        }
        if (sendto(socket_fd, message.data(), message_size, MSG_DONTWAIT,
                   reinterpret_cast<sockaddr*>(&peer), peer_size) ==
            static_cast<ssize_t>(message_size)) {
          ++host_to_guest;
        }
      }
    }
  }

  std::cout << "frames host->guest " << host_to_guest << ", guest->host "
            << guest_to_host << '\n';
  close(tap_fd);
  close(socket_fd);
  unlink(options.socket.c_str());
  return 0;
}
