#pragma once

#include <mikos/base.hpp>
#include <mikos/io/ring_buffer.hpp>

namespace mikos::process_model {

enum class PipeEnd : u8 { read, write };

struct PipeHandle {
  u16 generation{};
  u8 slot{0xff};

  [[nodiscard]] constexpr bool operator==(const PipeHandle&) const = default;
};

enum class PipeStatus : u8 {
  success,
  would_block,
  end_of_file,
  broken_pipe,
  bad_handle,
  no_space,
};

struct PipeIoResult {
  PipeStatus status{PipeStatus::bad_handle};
  u32 size{};
};

template <u32 TableCapacity = 8, u32 BufferCapacity = 4096,
          u32 AtomicCapacity = 4096>
class PipeTable {
 public:
  static_assert(TableCapacity != 0 && TableCapacity <= 255);
  static_assert(AtomicCapacity <= BufferCapacity);

  struct CreateResult {
    PipeStatus status{PipeStatus::no_space};
    PipeHandle handle{};
  };

  [[nodiscard]] constexpr CreateResult create() {
    for (u32 i = 0; i < TableCapacity; ++i) {
      auto& pipe = pipes_[i];
      if (!pipe.used) {
        u16 generation = static_cast<u16>(pipe.generation + 1);
        if (generation == 0) {
          generation = 1;
        }
        pipe = {};
        pipe.used = true;
        pipe.generation = generation;
        pipe.readers = 1;
        pipe.writers = 1;
        return {PipeStatus::success,
                PipeHandle{generation, static_cast<u8>(i)}};
      }
    }
    return {};
  }

  [[nodiscard]] constexpr PipeStatus retain(PipeHandle handle, PipeEnd end) {
    auto* pipe = find(handle);
    if (pipe == nullptr) {
      return PipeStatus::bad_handle;
    }
    u16& references = end == PipeEnd::read ? pipe->readers : pipe->writers;
    if (references == 0xffff) {
      return PipeStatus::no_space;
    }
    ++references;
    return PipeStatus::success;
  }

  [[nodiscard]] constexpr PipeStatus release(PipeHandle handle, PipeEnd end) {
    auto* pipe = find(handle);
    if (pipe == nullptr) {
      return PipeStatus::bad_handle;
    }
    u16& references = end == PipeEnd::read ? pipe->readers : pipe->writers;
    if (references == 0) {
      return PipeStatus::bad_handle;
    }
    --references;
    if (pipe->readers == 0 && pipe->writers == 0) {
      const u16 generation = pipe->generation;
      *pipe = {};
      pipe->generation = generation;
    }
    return PipeStatus::success;
  }

  [[nodiscard]] constexpr PipeIoResult read(PipeHandle handle, u8* output,
                                             u32 count) {
    auto* pipe = find(handle);
    if (pipe == nullptr || pipe->readers == 0) {
      return {};
    }
    if (count == 0) {
      return {PipeStatus::success, 0};
    }
    if (!pipe->data.empty()) {
      return {PipeStatus::success, pipe->data.read(output, count)};
    }
    return pipe->writers == 0
               ? PipeIoResult{PipeStatus::end_of_file, 0}
               : PipeIoResult{PipeStatus::would_block, 0};
  }

  [[nodiscard]] constexpr PipeIoResult write(PipeHandle handle,
                                              const u8* input, u32 count) {
    auto* pipe = find(handle);
    if (pipe == nullptr || pipe->writers == 0) {
      return {};
    }
    if (count == 0) {
      return {PipeStatus::success, 0};
    }
    if (pipe->readers == 0) {
      return {PipeStatus::broken_pipe, 0};
    }
    if (count <= AtomicCapacity && pipe->data.free_space() < count) {
      return {PipeStatus::would_block, 0};
    }
    const u32 amount = pipe->data.write(input, count);
    return amount == 0 ? PipeIoResult{PipeStatus::would_block, 0}
                       : PipeIoResult{PipeStatus::success, amount};
  }

  [[nodiscard]] constexpr bool readable(PipeHandle handle) const {
    const auto* pipe = find(handle);
    return pipe != nullptr && pipe->readers != 0 &&
           (!pipe->data.empty() || pipe->writers == 0);
  }

  [[nodiscard]] constexpr bool writable(PipeHandle handle) const {
    const auto* pipe = find(handle);
    return pipe != nullptr && pipe->writers != 0 && pipe->readers != 0 &&
           !pipe->data.full();
  }

  [[nodiscard]] constexpr bool hung_up(PipeHandle handle,
                                        PipeEnd end) const {
    const auto* pipe = find(handle);
    return pipe != nullptr &&
           (end == PipeEnd::read ? pipe->writers == 0 : pipe->readers == 0);
  }

  [[nodiscard]] constexpr u16 references(PipeHandle handle,
                                          PipeEnd end) const {
    const auto* pipe = find(handle);
    return pipe == nullptr
               ? 0
               : (end == PipeEnd::read ? pipe->readers : pipe->writers);
  }

  [[nodiscard]] constexpr u32 used() const {
    u32 count = 0;
    for (const auto& pipe : pipes_) {
      count += pipe.used ? 1u : 0u;
    }
    return count;
  }

 private:
  struct Pipe {
    io::RingBuffer<BufferCapacity> data{};
    u16 generation{};
    u16 readers{};
    u16 writers{};
    bool used{};
  };

  [[nodiscard]] constexpr Pipe* find(PipeHandle handle) {
    if (handle.slot >= TableCapacity) {
      return nullptr;
    }
    auto& pipe = pipes_[handle.slot];
    return pipe.used && pipe.generation == handle.generation ? &pipe
                                                             : nullptr;
  }

  [[nodiscard]] constexpr const Pipe* find(PipeHandle handle) const {
    if (handle.slot >= TableCapacity) {
      return nullptr;
    }
    const auto& pipe = pipes_[handle.slot];
    return pipe.used && pipe.generation == handle.generation ? &pipe
                                                             : nullptr;
  }

  Pipe pipes_[TableCapacity]{};
};

}  // namespace mikos::process_model
