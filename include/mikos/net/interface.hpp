#pragma once

#include <mikos/base.hpp>
#include <mikos/net/ethernet.hpp>

namespace mikos::network {

inline constexpr u32 interface_name_size = 16;
inline constexpr u32 interface_index = 1;

inline constexpr u16 interface_up = 0x0001;
inline constexpr u16 interface_broadcast = 0x0002;
inline constexpr u16 interface_running = 0x0040;
inline constexpr u16 interface_multicast = 0x1000;

inline constexpr u32 siocgifflags = 0x8913;
inline constexpr u32 siocgifconf = 0x8912;
inline constexpr u32 siocsifflags = 0x8914;
inline constexpr u32 siocgifaddr = 0x8915;
inline constexpr u32 siocsifaddr = 0x8916;
inline constexpr u32 siocgifbrdaddr = 0x8919;
inline constexpr u32 siocsifbrdaddr = 0x891a;
inline constexpr u32 siocgifnetmask = 0x891b;
inline constexpr u32 siocsifnetmask = 0x891c;
inline constexpr u32 siocgifmetric = 0x891d;
inline constexpr u32 siocgifmtu = 0x8921;
inline constexpr u32 siocgifhwaddr = 0x8927;
inline constexpr u32 siocgifindex = 0x8933;
inline constexpr u32 siocgiftxqlen = 0x8942;

struct [[gnu::packed]] Sockaddr32 {
  u16 family;
  u8 data[14];
};

union IfreqValue32 {
  Sockaddr32 address;
  u16 flags;
  i32 index;
  i32 metric;
  i32 mtu;
  i32 queue_length;
  u8 bytes[16];
};

struct [[gnu::packed]] Ifreq32 {
  char name[interface_name_size];
  IfreqValue32 value;
};

struct [[gnu::packed]] Ifconf32 {
  i32 length;
  u32 buffer;
};

static_assert(sizeof(Sockaddr32) == 16);
static_assert(sizeof(IfreqValue32) == 16);
static_assert(sizeof(Ifreq32) == 32);
static_assert(sizeof(Ifconf32) == 8);

struct InterfaceState {
  MacAddress mac{{0, 0, 0, 0, 0, 0}};
  Ipv4Address address{{10, 0, 2, 15}};
  Ipv4Address netmask{{255, 255, 255, 0}};
  Ipv4Address broadcast{{10, 0, 2, 255}};
  u16 flags{static_cast<u16>(interface_up | interface_broadcast |
                             interface_running | interface_multicast)};
};

enum class InterfaceControlResult : u8 {
  success,
  no_device,
  invalid_argument,
  unsupported,
};

[[nodiscard]] constexpr bool interface_name_matches(const char* name) {
  return name[0] == 'e' && name[1] == 't' && name[2] == 'h' &&
         name[3] == '0' && name[4] == '\0';
}

constexpr void write_interface_name(char* name) {
  for (u32 i = 0; i < interface_name_size; ++i) {
    name[i] = '\0';
  }
  name[0] = 'e';
  name[1] = 't';
  name[2] = 'h';
  name[3] = '0';
}

[[nodiscard]] constexpr Ipv4Address sockaddr_ipv4(
    const Sockaddr32& address) {
  return Ipv4Address{{address.data[2], address.data[3], address.data[4],
                      address.data[5]}};
}

constexpr void write_sockaddr_ipv4(Sockaddr32& output,
                                   Ipv4Address address) {
  output = {};
  output.family = 2;
  for (u32 i = 0; i < 4; ++i) {
    output.data[i + 2] = address.octet[i];
  }
}

constexpr void update_broadcast(InterfaceState& state) {
  for (u32 i = 0; i < 4; ++i) {
    state.broadcast.octet[i] = static_cast<u8>(
        state.address.octet[i] | static_cast<u8>(~state.netmask.octet[i]));
  }
}

[[nodiscard]] constexpr InterfaceControlResult apply_interface_ioctl(
    InterfaceState& state, u32 request, Ifreq32& interface_request) {
  if (!interface_name_matches(interface_request.name)) {
    return InterfaceControlResult::no_device;
  }
  switch (request) {
    case siocgifindex:
      interface_request.value.index = static_cast<i32>(interface_index);
      return InterfaceControlResult::success;
    case siocgifflags:
      interface_request.value.flags = state.flags;
      return InterfaceControlResult::success;
    case siocgifmetric:
      interface_request.value.metric = 0;
      return InterfaceControlResult::success;
    case siocgifmtu:
      interface_request.value.mtu = 1500;
      return InterfaceControlResult::success;
    case siocgiftxqlen:
      interface_request.value.queue_length = 1000;
      return InterfaceControlResult::success;
    case siocsifflags:
      state.flags = interface_request.value.flags;
      return InterfaceControlResult::success;
    case siocgifaddr:
      write_sockaddr_ipv4(interface_request.value.address, state.address);
      return InterfaceControlResult::success;
    case siocsifaddr:
      if (interface_request.value.address.family != 2) {
        return InterfaceControlResult::invalid_argument;
      }
      state.address = sockaddr_ipv4(interface_request.value.address);
      update_broadcast(state);
      return InterfaceControlResult::success;
    case siocgifnetmask:
      write_sockaddr_ipv4(interface_request.value.address, state.netmask);
      return InterfaceControlResult::success;
    case siocsifnetmask:
      if (interface_request.value.address.family != 2) {
        return InterfaceControlResult::invalid_argument;
      }
      state.netmask = sockaddr_ipv4(interface_request.value.address);
      update_broadcast(state);
      return InterfaceControlResult::success;
    case siocgifbrdaddr:
      write_sockaddr_ipv4(interface_request.value.address, state.broadcast);
      return InterfaceControlResult::success;
    case siocsifbrdaddr:
      if (interface_request.value.address.family != 2) {
        return InterfaceControlResult::invalid_argument;
      }
      state.broadcast = sockaddr_ipv4(interface_request.value.address);
      return InterfaceControlResult::success;
    case siocgifhwaddr:
      interface_request.value.address = {};
      interface_request.value.address.family = 1;
      for (u32 i = 0; i < 6; ++i) {
        interface_request.value.address.data[i] = state.mac.octet[i];
      }
      return InterfaceControlResult::success;
    default:
      return InterfaceControlResult::unsupported;
  }
}

}  // namespace mikos::network
