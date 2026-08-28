#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <utility>
#include <vector>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
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
  bool self_test = false;
  std::string probe_socket;
  std::string instance_tag;
};

void usage(const char* program) {
  std::cerr
      << "usage: " << program
      << " [--tap NAME] [--socket PATH] [--peer QEMU_SOCKET] [--self-test]\n"
         "       [--probe-socket PATH]\n"
         "       [--instance-tag TEXT]\n"
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
    } else if (std::strcmp(argv[i], "--self-test") == 0) {
      options.self_test = true;
    } else if (std::strcmp(argv[i], "--probe-socket") == 0 &&
               i + 1 < argc) {
      options.probe_socket = argv[++i];
    } else if (std::strcmp(argv[i], "--instance-tag") == 0 && i + 1 < argc) {
      options.instance_tag = argv[++i];
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
  // This bridge normally runs as root because it owns a TAP device, while the
  // cycle-level simulator deliberately runs as the invoking user.  bind(2)
  // applies root's umask and would otherwise leave a 0755 socket that the
  // simulator cannot connect to.  The pathname is protected from replacement
  // by /tmp's sticky bit; make the datagram endpoint usable by the client.
  if (chmod(path.c_str(), 0666) != 0) {
    std::cerr << "chmod " << path << ": " << std::strerror(errno) << '\n';
    close(fd);
    unlink(path.c_str());
    return -1;
  }
  return fd;
}

bool probe_socket(const std::string& path) {
  sockaddr_un address{};
  if (!make_unix_address(path, address)) return false;
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::cerr << "probe socket: " << std::strerror(errno) << '\n';
    return false;
  }
  const bool ok = connect(fd, reinterpret_cast<sockaddr*>(&address),
                          sizeof(address)) == 0;
  if (!ok) {
    std::cerr << "connect " << path << ": " << std::strerror(errno) << '\n';
  }
  close(fd);
  return ok;
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

constexpr size_t bridge_queue_capacity = 64;

bool mikos_frame_supported(const uint8_t* frame, size_t size) {
  return size >= 14 && frame[12] == 0x08 &&
         (frame[13] == 0x00 || frame[13] == 0x06);
}

bool mikos_frame_priority(const uint8_t* frame, size_t size) {
  if (size < 14) return false;
  if (frame[12] == 0x08 && frame[13] == 0x06) return true;
  if (frame[12] != 0x08 || frame[13] != 0x00 || size < 34) return false;
  const size_t ip_header = (frame[14] & 0x0f) * 4;
  if (ip_header < 20 || size < 14 + ip_header || frame[23] != 6) return false;
  const size_t ip_length = (static_cast<size_t>(frame[16]) << 8) | frame[17];
  if (ip_length < ip_header + 20 || size < 14 + ip_length) return false;
  const size_t tcp = 14 + ip_header;
  const size_t tcp_header = (frame[tcp + 12] >> 4) * 4;
  if (tcp_header < 20 || ip_length < ip_header + tcp_header) return false;
  const size_t payload = ip_length - ip_header - tcp_header;
  return payload != 0 || (frame[tcp + 13] & 0x07) != 0;
}

struct QueuedFrame {
  std::vector<uint8_t> bytes;
  bool priority = false;
};

bool enqueue_frame(std::deque<QueuedFrame>& queue, std::vector<uint8_t> frame,
                   bool priority, uint64_t& dropped) {
  if (queue.size() >= bridge_queue_capacity) {
    auto victim = queue.end();
    for (auto candidate = queue.begin(); candidate != queue.end(); ++candidate) {
      if (!candidate->priority) {
        victim = candidate;
        break;
      }
    }
    if (victim == queue.end() && priority) victim = queue.begin();
    if (victim == queue.end()) {
      ++dropped;
      return false;
    }
    queue.erase(victim);
    ++dropped;
  }
  queue.push_back({std::move(frame), priority});
  return true;
}

void enlarge_socket_buffers(int fd) {
  constexpr int bytes = 4 * 1024 * 1024;
  static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)));
  static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)));
}

bool bridge_self_test() {
  bool ok = true;

  const std::string socket_path =
      "/tmp/mikos-ethgig-self-test-" + std::to_string(getpid()) + ".sock";
  const int socket_fd = open_socket(socket_path);
  struct stat socket_status {};
  if (socket_fd < 0 || stat(socket_path.c_str(), &socket_status) != 0 ||
      !S_ISSOCK(socket_status.st_mode) ||
      (socket_status.st_mode & 0777) != 0666 || !probe_socket(socket_path)) {
    std::cerr << "bridge self-test: client-accessible socket setup failed\n";
    ok = false;
  }
  if (socket_fd >= 0) close(socket_fd);
  unlink(socket_path.c_str());

  std::vector<uint8_t> ack(54);
  ack[12] = 0x08;
  ack[13] = 0x00;
  ack[14] = 0x45;
  ack[17] = 40;
  ack[23] = 6;
  ack[46] = 0x50;
  ack[47] = 0x10;
  auto data = ack;
  data.push_back(0x42);
  data[17] = 41;
  auto syn = ack;
  syn[47] = 0x02;
  auto arp = ack;
  arp.resize(42);
  arp[13] = 0x06;
  auto ipv6 = ack;
  ipv6[12] = 0x86;
  ipv6[13] = 0xdd;
  if (!mikos_frame_supported(ack.data(), ack.size()) ||
      !mikos_frame_supported(arp.data(), arp.size()) ||
      mikos_frame_supported(ipv6.data(), ipv6.size()) ||
      mikos_frame_priority(ack.data(), ack.size()) ||
      !mikos_frame_priority(data.data(), data.size()) ||
      !mikos_frame_priority(syn.data(), syn.size()) ||
      !mikos_frame_priority(arp.data(), arp.size())) {
    std::cerr << "bridge self-test: Ethernet/TCP classification failed\n";
    ok = false;
  }

  std::deque<QueuedFrame> queue;
  uint64_t dropped = 0;
  for (size_t index = 0; index < bridge_queue_capacity; ++index) {
    enqueue_frame(queue, {static_cast<uint8_t>(index)}, true, dropped);
  }
  if (enqueue_frame(queue, {0xee}, false, dropped) ||
      queue.size() != bridge_queue_capacity || dropped != 1) {
    std::cerr << "bridge self-test: ACK displaced full payload queue\n";
    ok = false;
  }
  queue.clear();
  dropped = 0;
  enqueue_frame(queue, {0xaa}, false, dropped);
  for (size_t index = 1; index < bridge_queue_capacity; ++index) {
    enqueue_frame(queue, {static_cast<uint8_t>(index)}, true, dropped);
  }
  if (!enqueue_frame(queue, {0xff}, true, dropped) || queue.front().bytes[0] == 0xaa ||
      queue.back().bytes[0] != 0xff || dropped != 1) {
    std::cerr << "bridge self-test: payload did not evict recoverable ACK\n";
    ok = false;
  }
  return ok;
}

void write_pcap_header(FILE* output) {
  // Native-endian libpcap header, Ethernet link type.
  const uint32_t header[]{0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1};
  static_cast<void>(fwrite(header, sizeof(header), 1, output));
}

void write_pcap_frame(FILE* output, const uint8_t* frame, size_t size) {
  if (output == nullptr || size > UINT32_MAX) return;
  timeval now{};
  gettimeofday(&now, nullptr);
  const uint32_t length = static_cast<uint32_t>(size);
  const uint32_t header[]{static_cast<uint32_t>(now.tv_sec),
                          static_cast<uint32_t>(now.tv_usec), length, length};
  static_cast<void>(fwrite(header, sizeof(header), 1, output));
  static_cast<void>(fwrite(frame, size, 1, output));
  fflush(output);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 2;
  }
  if (!options.probe_socket.empty()) {
    return probe_socket(options.probe_socket) ? 0 : 1;
  }
  if (options.self_test) return bridge_self_test() ? 0 : 1;
  const int socket_fd = open_socket(options.socket);
  if (socket_fd < 0) {
    return 1;
  }
  enlarge_socket_buffers(socket_fd);
  const int tap_fd = open_tap(options);
  if (tap_fd < 0) {
    close(socket_fd);
    unlink(options.socket.c_str());
    return 1;
  }
  FILE* pcap = nullptr;
  if (const char* path = std::getenv("MIKOS_ETH_TAP_PCAP")) {
    pcap = std::fopen(path, "wb");
    if (pcap == nullptr) {
      std::cerr << "open packet capture " << path << ": "
                << std::strerror(errno) << '\n';
      close(tap_fd);
      close(socket_fd);
      unlink(options.socket.c_str());
      return 1;
    }
    write_pcap_header(pcap);
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
  uint64_t host_to_guest_dropped = 0;
  uint64_t guest_to_host_dropped = 0;
  std::deque<QueuedFrame> pending_to_guest;
  std::deque<QueuedFrame> pending_to_host;

  while (!stop_requested) {
    pollfd descriptors[2]{{tap_fd, static_cast<short>(
                                      POLLIN | (pending_to_host.empty()
                                                   ? 0
                                                   : POLLOUT)), 0},
                          {socket_fd, POLLIN, 0}};
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
            // A HELLO starts a new simulator generation. Never deliver frames
            // retained for the pathname of a simulator that already exited.
            pending_to_guest.clear();
            pending_to_host.clear();
            continue;
          }
          if (received < 3 || message[0] != 2) continue;
          frame_size = (static_cast<size_t>(message[1]) << 8) | message[2];
          if (frame_size > static_cast<size_t>(received - 3)) continue;
          frame += 3;
        }
        write_pcap_frame(pcap, frame, frame_size);
        std::vector<uint8_t> queued(frame, frame + frame_size);
        static_cast<void>(enqueue_frame(
            pending_to_host, std::move(queued),
            mikos_frame_priority(frame, frame_size), guest_to_host_dropped));
      }
    }

    while (!pending_to_host.empty()) {
      const auto& frame = pending_to_host.front().bytes;
      const ssize_t written = write(tap_fd, frame.data(), frame.size());
      if (written == static_cast<ssize_t>(frame.size())) {
        pending_to_host.pop_front();
        ++guest_to_host;
        continue;
      }
      if (written < 0 && transient_error()) break;
      std::cerr << "write TAP: "
                << (written < 0 ? std::strerror(errno) : "short frame write")
                << '\n';
      pending_to_host.pop_front();
      ++guest_to_host_dropped;
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
        const size_t frame_size = static_cast<size_t>(received);
        write_pcap_frame(pcap, frame, frame_size);
        if (!mikos_frame_supported(frame, frame_size)) continue;
        if (!have_peer) continue;
        size_t message_size = frame_size;
        if (!qemu_mode) {
          message[0] = 2;
          message[1] = static_cast<uint8_t>(received >> 8);
          message[2] = static_cast<uint8_t>(received);
          message_size += 3;
        }
        std::vector<uint8_t> queued(message.begin(),
                                    message.begin() + message_size);
        static_cast<void>(enqueue_frame(
            pending_to_guest, std::move(queued),
            mikos_frame_priority(frame, frame_size), host_to_guest_dropped));
      }
    }

    while (have_peer && !pending_to_guest.empty()) {
      const auto& message = pending_to_guest.front().bytes;
      const ssize_t sent = sendto(
          socket_fd, message.data(), message.size(), MSG_DONTWAIT,
          reinterpret_cast<sockaddr*>(&peer), peer_size);
      if (sent == static_cast<ssize_t>(message.size())) {
        pending_to_guest.pop_front();
        ++host_to_guest;
        continue;
      }
      if (sent < 0 && transient_error()) break;
      std::cerr << "sendto simulator: "
                << (sent < 0 ? std::strerror(errno) : "short datagram write")
                << '\n';
      pending_to_guest.pop_front();
      ++host_to_guest_dropped;
    }
  }

  std::cout << "frames host->guest " << host_to_guest << ", guest->host "
            << guest_to_host << ", host->guest dropped "
            << host_to_guest_dropped << ", guest->host dropped "
            << guest_to_host_dropped << '\n';
  close(tap_fd);
  close(socket_fd);
  if (pcap != nullptr) fclose(pcap);
  unlink(options.socket.c_str());
  return 0;
}
