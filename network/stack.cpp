#include <mikos/drivers/net.hpp>
#include <mikos/kernel.hpp>

namespace mikos::network {
namespace {

enum class Reply : u8 {
  none,
  arp,
  echo,
};

class BootstrapStack {
 public:
  [[nodiscard]] bool run() {
    if (!drivers::net::initialize()) {
      write_text("MIKOS:NET_NONE\n");
      return false;
    }
    mac_ = drivers::net::mac_address();
    announce_address();

    for (u32 spin = 0; spin < 100'000'000; ++spin) {
      drivers::net::Frame frame{};
      if (!drivers::net::receive(frame)) {
        continue;
      }
      record(handle(frame));
      if (arp_replied_ && echo_replied_) {
        return true;
      }
    }
    write_text("MIKOS:NET_TIMEOUT\n");
    return false;
  }

 private:
  inline static constexpr Ipv4Address address{{10, 0, 2, 15}};

  [[nodiscard]] Reply handle(drivers::net::Frame frame) const {
    if (const u32 size = make_arp_reply(frame.data, frame.size, mac_, address);
        size != 0) {
      return drivers::net::transmit(frame.data, size) ? Reply::arp
                                                       : Reply::none;
    }
    if (const u32 size =
            make_icmp_echo_reply(frame.data, frame.size, mac_, address);
        size != 0) {
      return drivers::net::transmit(frame.data, size) ? Reply::echo
                                                       : Reply::none;
    }
    return Reply::none;
  }

  void record(Reply reply) {
    switch (reply) {
      case Reply::arp:
        arp_replied_ = true;
        write_text("MIKOS:ARP_REPLY\n");
        break;
      case Reply::echo:
        echo_replied_ = true;
        write_text("MIKOS:ICMP_ECHO_REPLY\n");
        break;
      case Reply::none:
        break;
    }
  }

  void announce_address() const {
    write_text("MIKOS:NET_IP 10.0.2.15\n");
    write_text("MIKOS:NET_MAC ");
    constexpr char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 6; ++i) {
      uart_put(hex[mac_.octet[i] >> 4]);
      uart_put(hex[mac_.octet[i] & 0x0f]);
      uart_put(i == 5 ? '\n' : ':');
    }
  }

  MacAddress mac_{};
  bool arp_replied_{};
  bool echo_replied_{};
};

BootstrapStack bootstrap_stack;

}  // namespace

bool boot_probe() { return bootstrap_stack.run(); }

}  // namespace mikos::network
