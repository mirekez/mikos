#include <mikos/net/interface.hpp>

#include <support/test.hpp>

namespace {

[[nodiscard]] mikos::network::Ifreq32 request_for_eth0() {
  mikos::network::Ifreq32 request{};
  mikos::network::write_interface_name(request.name);
  return request;
}

[[nodiscard]] bool address_is(const mikos::network::Sockaddr32& address,
                              mikos::u8 a, mikos::u8 b, mikos::u8 c,
                              mikos::u8 d) {
  return address.family == 2 && address.data[2] == a &&
         address.data[3] == b && address.data[4] == c &&
         address.data[5] == d;
}

}  // namespace

int main() {
  mikos::test::Suite suite{"kernel/ioctl_syscall"};
  using mikos::network::InterfaceControlResult;

  mikos::network::InterfaceState state{};
  state.mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};

  auto request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifindex, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, request.value.index == 1);

  request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifflags, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, (request.value.flags & mikos::network::interface_up) != 0);

  request = request_for_eth0();
  request.value.flags = static_cast<mikos::u16>(
      mikos::network::interface_broadcast | mikos::network::interface_running);
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocsifflags, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, state.flags == static_cast<mikos::u16>(request.value.flags));

  request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifaddr, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, address_is(request.value.address, 10, 0, 2, 15));

  request = request_for_eth0();
  mikos::network::write_sockaddr_ipv4(
      request.value.address, mikos::Ipv4Address{{192, 168, 76, 2}});
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocsifaddr, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite,
              (state.address == mikos::Ipv4Address{{192, 168, 76, 2}}));
  MIKOS_CHECK(suite,
              (state.broadcast == mikos::Ipv4Address{{192, 168, 76, 255}}));

  request = request_for_eth0();
  mikos::network::write_sockaddr_ipv4(
      request.value.address, mikos::Ipv4Address{{255, 255, 255, 128}});
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocsifnetmask, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite,
              (state.netmask == mikos::Ipv4Address{{255, 255, 255, 128}}));
  MIKOS_CHECK(suite,
              (state.broadcast == mikos::Ipv4Address{{192, 168, 76, 127}}));

  request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifnetmask, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, address_is(request.value.address, 255, 255, 255, 128));

  request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifbrdaddr, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, address_is(request.value.address, 192, 168, 76, 127));

  request = request_for_eth0();
  mikos::network::write_sockaddr_ipv4(
      request.value.address, mikos::Ipv4Address{{192, 168, 76, 126}});
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocsifbrdaddr, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite,
              (state.broadcast == mikos::Ipv4Address{{192, 168, 76, 126}}));

  request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifhwaddr, request) ==
                         InterfaceControlResult::success);
  MIKOS_CHECK(suite, request.value.address.family == 1);
  MIKOS_CHECK(suite, request.value.address.data[0] == 0x02);
  MIKOS_CHECK(suite, request.value.address.data[5] == 0x02);

  request = request_for_eth0();
  request.value.address.family = 10;
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocsifaddr, request) ==
                         InterfaceControlResult::invalid_argument);

  request = request_for_eth0();
  request.name[3] = '1';
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, mikos::network::siocgifindex, request) ==
                         InterfaceControlResult::no_device);

  request = request_for_eth0();
  MIKOS_CHECK(suite, mikos::network::apply_interface_ioctl(
                         state, 0xffffffff, request) ==
                         InterfaceControlResult::unsupported);

  return suite.finish();
}
