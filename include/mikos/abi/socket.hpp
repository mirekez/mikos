#pragma once

#include <mikos/base.hpp>

namespace mikos::abi::socket {

inline constexpr u32 af_inet = 2;
inline constexpr u32 sock_stream = 1;
inline constexpr u32 sock_dgram = 2;
inline constexpr u32 sock_nonblock = 0x800;
inline constexpr u32 sock_cloexec = 0x80000;
inline constexpr u32 ipproto_tcp = 6;

struct [[gnu::packed]] SockaddrIn {
  u16 family;
  u16 port;
  u8 address[4];
  u8 zero[8];
};

static_assert(sizeof(SockaddrIn) == 16);

enum class Type : u8 {
  stream,
  datagram,
};

enum class ValidationResult : u8 {
  success,
  address_family_not_supported,
  socket_type_not_supported,
  protocol_not_supported,
};

[[nodiscard]] constexpr ValidationResult validate(u32 domain, u32 type,
                                                  u32 protocol) {
  if (domain != af_inet) {
    return ValidationResult::address_family_not_supported;
  }
  constexpr u32 type_flags = sock_nonblock | sock_cloexec;
  const u32 base_type = type & ~type_flags;
  if (base_type != sock_stream && base_type != sock_dgram) {
    return ValidationResult::socket_type_not_supported;
  }
  if (protocol != 0 &&
      !(base_type == sock_stream && protocol == ipproto_tcp)) {
    return ValidationResult::protocol_not_supported;
  }
  return ValidationResult::success;
}

[[nodiscard]] constexpr Type type(u32 value) {
  return (value & ~(sock_nonblock | sock_cloexec)) == sock_stream
             ? Type::stream
             : Type::datagram;
}

}  // namespace mikos::abi::socket
