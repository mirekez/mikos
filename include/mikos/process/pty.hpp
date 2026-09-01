#pragma once

#include <mikos/base.hpp>
#include <mikos/io/ring_buffer.hpp>
#include <mikos/process/signal.hpp>

namespace mikos::process_model {

enum class PtyEnd : u8 { master, slave };

struct PtyHandle {
  u16 generation{};
  u8 slot{0xff};

  [[nodiscard]] constexpr bool operator==(const PtyHandle&) const = default;
};

enum class PtyStatus : u8 {
  success,
  would_block,
  end_of_file,
  io_error,
  bad_handle,
  locked,
  no_space,
  permission_denied,
  invalid_argument,
};

struct PtyIoResult {
  PtyStatus status{PtyStatus::bad_handle};
  u32 size{};
  u64 generated_signals{};
};

struct TermiosState {
  u32 input_flags{0x00000500};
  u32 output_flags{0x00000005};
  u32 control_flags{0x000000bf};
  u32 local_flags{0x0000803b};
  u8 line{};
  u8 control[19]{3, 28, 127, 21, 4, 0, 1, 0, 17, 19,
                 26, 0, 18,  15, 23, 22, 0, 0, 0};

  [[nodiscard]] constexpr bool operator==(const TermiosState&) const = default;
};

struct WindowSize {
  u16 rows{24};
  u16 columns{80};
  u16 horizontal_pixels{};
  u16 vertical_pixels{};

  [[nodiscard]] constexpr bool operator==(const WindowSize&) const = default;
};

template <u32 TableCapacity = 4, u32 BufferCapacity = 4096>
class PtyTable {
 public:
  static_assert(TableCapacity != 0 && TableCapacity <= 255);

  struct OpenResult {
    PtyStatus status{PtyStatus::no_space};
    PtyHandle handle{};
  };

  [[nodiscard]] constexpr OpenResult open_master() {
    for (u32 i = 0; i < TableCapacity; ++i) {
      auto& pty = ptys_[i];
      if (!pty.used) {
        u16 generation = static_cast<u16>(pty.generation + 1);
        if (generation == 0) {
          generation = 1;
        }
        pty = {};
        pty.used = true;
        pty.locked = true;
        pty.generation = generation;
        pty.masters = 1;
        return {PtyStatus::success,
                PtyHandle{generation, static_cast<u8>(i)}};
      }
    }
    return {};
  }

  [[nodiscard]] constexpr PtyStatus set_locked(PtyHandle handle,
                                                bool locked) {
    auto* pty = find(handle);
    if (pty == nullptr || pty->masters == 0) {
      return PtyStatus::bad_handle;
    }
    pty->locked = locked;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr OpenResult open_slave(u8 number) {
    if (number >= TableCapacity || !ptys_[number].used) {
      return {PtyStatus::bad_handle, {}};
    }
    auto& pty = ptys_[number];
    if (pty.locked) {
      return {PtyStatus::locked, {}};
    }
    if (pty.slaves == 0xffff) {
      return {};
    }
    ++pty.slaves;
    return {PtyStatus::success,
            PtyHandle{pty.generation, static_cast<u8>(number)}};
  }

  [[nodiscard]] constexpr OpenResult open_controlling_slave(u32 session) {
    if (session == 0) {
      return {PtyStatus::bad_handle, {}};
    }
    for (u32 i = 0; i < TableCapacity; ++i) {
      auto& pty = ptys_[i];
      if (!pty.used || pty.session != session) {
        continue;
      }
      if (pty.locked) {
        return {PtyStatus::locked, {}};
      }
      if (pty.slaves == 0xffff) {
        return {};
      }
      ++pty.slaves;
      return {PtyStatus::success,
              PtyHandle{pty.generation, static_cast<u8>(i)}};
    }
    return {PtyStatus::bad_handle, {}};
  }

  [[nodiscard]] constexpr PtyStatus retain(PtyHandle handle, PtyEnd end) {
    auto* pty = find(handle);
    if (pty == nullptr) {
      return PtyStatus::bad_handle;
    }
    u16& references = end == PtyEnd::master ? pty->masters : pty->slaves;
    if (references == 0 || references == 0xffff) {
      return references == 0 ? PtyStatus::bad_handle : PtyStatus::no_space;
    }
    ++references;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr PtyStatus release(PtyHandle handle, PtyEnd end) {
    auto* pty = find(handle);
    if (pty == nullptr) {
      return PtyStatus::bad_handle;
    }
    u16& references = end == PtyEnd::master ? pty->masters : pty->slaves;
    if (references == 0) {
      return PtyStatus::bad_handle;
    }
    --references;
    if (pty->masters == 0 && pty->slaves == 0) {
      const u16 generation = pty->generation;
      *pty = {};
      pty->generation = generation;
    }
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr PtyIoResult read(PtyHandle handle, PtyEnd end,
                                            u8* output, u32 count) {
    auto* pty = find(handle);
    if (pty == nullptr || references(*pty, end) == 0) {
      return {};
    }
    if (count == 0) {
      return {PtyStatus::success, 0};
    }
    auto& ring = end == PtyEnd::master ? pty->slave_to_master
                                       : pty->master_to_slave;
    if (!ring.empty()) {
      return {PtyStatus::success, ring.read(output, count)};
    }
    const bool peer_closed =
        end == PtyEnd::master ? pty->slaves == 0 : pty->masters == 0;
    return peer_closed ? PtyIoResult{PtyStatus::end_of_file, 0}
                       : PtyIoResult{PtyStatus::would_block, 0};
  }

  [[nodiscard]] constexpr PtyIoResult write(PtyHandle handle, PtyEnd end,
                                             const u8* input, u32 count) {
    auto* pty = find(handle);
    if (pty == nullptr || references(*pty, end) == 0) {
      return {};
    }
    if (count == 0) {
      return {PtyStatus::success, 0};
    }
    const bool peer_closed =
        end == PtyEnd::master ? pty->slaves == 0 : pty->masters == 0;
    if (peer_closed) {
      return {PtyStatus::io_error, 0};
    }
    auto& ring = end == PtyEnd::master ? pty->master_to_slave
                                       : pty->slave_to_master;
    if (end == PtyEnd::slave) {
      // Output written through the slave passes through the terminal output
      // discipline. Linux PTYs start with OPOST | ONLCR, which turns a line
      // feed into CR-LF. Without that conversion a terminal moves down but
      // stays in the current column, producing increasingly indented output.
      constexpr u32 opost = 0x00000001;
      constexpr u32 onlcr = 0x00000004;
      const bool translate_newline =
          (pty->termios.output_flags & (opost | onlcr)) == (opost | onlcr);
      u32 consumed = 0;
      while (consumed < count) {
        const u8 character = input[consumed];
        if (translate_newline && character == '\n') {
          // Treat the expansion atomically. Returning a partial CR here
          // would make a retry of the same input byte produce CR-CR-LF.
          if (ring.free_space() < 2) {
            break;
          }
          const u8 newline[]{'\r', '\n'};
          static_cast<void>(ring.write(newline, sizeof(newline)));
        } else {
          if (ring.full()) {
            break;
          }
          static_cast<void>(ring.write(&character, 1));
        }
        ++consumed;
      }
      return consumed == 0
                 ? PtyIoResult{PtyStatus::would_block, 0}
                 : PtyIoResult{PtyStatus::success, consumed};
    }

    // Input written to a PTY master passes through the slave's line
    // discipline. Honour the signal-generating characters even though the
    // rest of MikOS's bounded line discipline is deliberately minimal.
    constexpr u32 isig = 0x00000001;
    constexpr u32 echo = 0x00000008;
    constexpr u32 noflsh = 0x00000080;
    constexpr u32 echoctl = 0x00000200;
    constexpr u8 vintr = 0;
    constexpr u8 vquit = 1;
    constexpr u8 vsusp = 10;
    constexpr u8 disabled = 0;
    u32 consumed = 0;
    u64 generated_signals = 0;
    for (; consumed < count; ++consumed) {
      const u8 character = input[consumed];
      u8 signal = 0;
      if ((pty->termios.local_flags & isig) != 0) {
        if (pty->termios.control[vintr] != disabled &&
            character == pty->termios.control[vintr]) {
          signal = signal_interrupt;
        } else if (pty->termios.control[vquit] != disabled &&
                   character == pty->termios.control[vquit]) {
          signal = signal_quit;
        } else if (pty->termios.control[vsusp] != disabled &&
                   character == pty->termios.control[vsusp]) {
          signal = signal_tty_stop;
        }
      }
      if (signal == 0) {
        if (ring.full()) {
          break;
        }
        static_cast<void>(ring.write(&character, 1));
        continue;
      }

      generated_signals |= SignalState::bit(signal);
      if ((pty->termios.local_flags & noflsh) == 0) {
        pty->master_to_slave.clear();
        pty->slave_to_master.clear();
      }
      if ((pty->termios.local_flags & echo) != 0) {
        if ((pty->termios.local_flags & echoctl) != 0 && character < 32) {
          const u8 visible[]{'^', static_cast<u8>(character + '@')};
          static_cast<void>(pty->slave_to_master.write(visible, 2));
        } else {
          static_cast<void>(pty->slave_to_master.write(&character, 1));
        }
      }
    }
    return consumed == 0
               ? PtyIoResult{PtyStatus::would_block, 0, generated_signals}
               : PtyIoResult{PtyStatus::success, consumed,
                             generated_signals};
  }

  [[nodiscard]] constexpr PtyStatus acquire_controlling_terminal(
      PtyHandle handle, u32 pid, u32 process_group, u32 session,
      bool force = false) {
    auto* pty = find(handle);
    if (pty == nullptr || pty->slaves == 0) {
      return PtyStatus::bad_handle;
    }
    if (pid != session) {
      return PtyStatus::permission_denied;
    }
    if (pty->session != 0 && pty->session != session && !force) {
      return PtyStatus::permission_denied;
    }
    pty->session = session;
    pty->foreground_group = process_group;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr PtyStatus detach(PtyHandle handle, u32 session) {
    auto* pty = find(handle);
    if (pty == nullptr) {
      return PtyStatus::bad_handle;
    }
    if (pty->session != session) {
      return PtyStatus::permission_denied;
    }
    pty->session = 0;
    pty->foreground_group = 0;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr PtyStatus set_foreground_group(PtyHandle handle,
                                                          u32 session,
                                                          u32 group) {
    auto* pty = find(handle);
    if (pty == nullptr) {
      return PtyStatus::bad_handle;
    }
    if (group == 0) {
      return PtyStatus::invalid_argument;
    }
    if (pty->session == 0 || pty->session != session) {
      return PtyStatus::permission_denied;
    }
    pty->foreground_group = group;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr u32 foreground_group(PtyHandle handle) const {
    const auto* pty = find(handle);
    return pty == nullptr ? 0 : pty->foreground_group;
  }

  [[nodiscard]] constexpr u32 session(PtyHandle handle) const {
    const auto* pty = find(handle);
    return pty == nullptr ? 0 : pty->session;
  }

  [[nodiscard]] constexpr PtyStatus set_termios(PtyHandle handle,
                                                 TermiosState value) {
    auto* pty = find(handle);
    if (pty == nullptr) {
      return PtyStatus::bad_handle;
    }
    pty->termios = value;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr const TermiosState* termios(
      PtyHandle handle) const {
    const auto* pty = find(handle);
    return pty == nullptr ? nullptr : &pty->termios;
  }

  [[nodiscard]] constexpr PtyStatus set_window(PtyHandle handle,
                                                WindowSize value,
                                                bool* changed = nullptr) {
    auto* pty = find(handle);
    if (pty == nullptr) {
      return PtyStatus::bad_handle;
    }
    if (changed != nullptr) {
      *changed = !(pty->window == value);
    }
    pty->window = value;
    return PtyStatus::success;
  }

  [[nodiscard]] constexpr const WindowSize* window(PtyHandle handle) const {
    const auto* pty = find(handle);
    return pty == nullptr ? nullptr : &pty->window;
  }

  [[nodiscard]] constexpr bool readable(PtyHandle handle, PtyEnd end) const {
    const auto* pty = find(handle);
    if (pty == nullptr || references(*pty, end) == 0) {
      return false;
    }
    const auto& ring = end == PtyEnd::master ? pty->slave_to_master
                                             : pty->master_to_slave;
    const bool peer_closed =
        end == PtyEnd::master ? pty->slaves == 0 : pty->masters == 0;
    return !ring.empty() || peer_closed;
  }

  [[nodiscard]] constexpr bool writable(PtyHandle handle, PtyEnd end) const {
    const auto* pty = find(handle);
    if (pty == nullptr || references(*pty, end) == 0) {
      return false;
    }
    const bool peer_closed =
        end == PtyEnd::master ? pty->slaves == 0 : pty->masters == 0;
    const auto& ring = end == PtyEnd::master ? pty->master_to_slave
                                             : pty->slave_to_master;
    return !peer_closed && !ring.full();
  }

  [[nodiscard]] constexpr u32 used() const {
    u32 count = 0;
    for (const auto& pty : ptys_) {
      count += pty.used ? 1u : 0u;
    }
    return count;
  }

  [[nodiscard]] constexpr bool active(u8 number) const {
    return number < TableCapacity && ptys_[number].used;
  }

  [[nodiscard]] constexpr u16 mode(u8 number) const {
    return active(number) ? ptys_[number].mode : 0;
  }

  [[nodiscard]] constexpr PtyStatus set_mode(u8 number, u16 mode) {
    if (!active(number)) {
      return PtyStatus::bad_handle;
    }
    ptys_[number].mode = static_cast<u16>(mode & 07777u);
    return PtyStatus::success;
  }

 private:
  struct Pty {
    io::RingBuffer<BufferCapacity> master_to_slave{};
    io::RingBuffer<BufferCapacity> slave_to_master{};
    TermiosState termios{};
    WindowSize window{};
    u32 session{};
    u32 foreground_group{};
    u16 generation{};
    u16 masters{};
    u16 slaves{};
    u16 mode{0666};
    bool used{};
    bool locked{};
  };

  [[nodiscard]] static constexpr u16 references(const Pty& pty, PtyEnd end) {
    return end == PtyEnd::master ? pty.masters : pty.slaves;
  }

  [[nodiscard]] constexpr Pty* find(PtyHandle handle) {
    if (handle.slot >= TableCapacity) {
      return nullptr;
    }
    auto& pty = ptys_[handle.slot];
    return pty.used && pty.generation == handle.generation ? &pty : nullptr;
  }

  [[nodiscard]] constexpr const Pty* find(PtyHandle handle) const {
    if (handle.slot >= TableCapacity) {
      return nullptr;
    }
    const auto& pty = ptys_[handle.slot];
    return pty.used && pty.generation == handle.generation ? &pty : nullptr;
  }

  Pty ptys_[TableCapacity]{};
};

}  // namespace mikos::process_model
