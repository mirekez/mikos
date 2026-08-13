#include <mikos/arch.hpp>
#include <drivers/fs/root.hpp>
#include <drivers/uart/uart.hpp>
#include <mikos/kernel.hpp>

extern "C" void* memcpy(void*, const void*, mikos::usize);
extern "C" void* memset(void*, int, mikos::usize);
extern "C" void trap_entry();
#ifdef MIKOS_TRIBE_INTERACTIVE
extern "C" unsigned char __embedded_busybox_begin[];
extern "C" unsigned char __embedded_busybox_end[];
extern "C" unsigned char __embedded_dropbear_begin[];
extern "C" unsigned char __embedded_dropbear_end[];
#endif

namespace mikos {
namespace {

#ifdef MIKOS_TRIBE
inline constexpr u32 plic_m_enable = 0x82012000;
inline constexpr u32 plic_s_enable = 0x82012080;
#else
inline constexpr u32 plic_m_enable = 0x0c002000;
inline constexpr u32 plic_s_enable = 0x0c002080;
#endif
#ifndef MIKOS_TRIBE
inline constexpr u32 sifive_test = 0x00100000;
#endif

struct [[gnu::packed]] ElfHeader {
  u8 ident[16];
  u16 type;
  u16 machine;
  u32 version;
  u32 entry;
  u32 program_offset;
  u32 section_offset;
  u32 flags;
  u16 header_size;
  u16 program_entry_size;
  u16 program_count;
  u16 section_entry_size;
  u16 section_count;
  u16 section_names;
};

struct [[gnu::packed]] ProgramHeader {
  u32 type;
  u32 offset;
  u32 virtual_address;
  u32 physical_address;
  u32 file_size;
  u32 memory_size;
  u32 flags;
  u32 alignment;
};

struct Image {
  u32 entry;
  u32 program_headers;
  u32 program_count;
  u32 brk;
  u32 mutable_begin;
};

struct Aux {
  u32 type;
  u32 value;
};

[[nodiscard]] bool valid_elf(const ElfHeader& header, u32 file_size) {
  return header.ident[0] == 0x7f && header.ident[1] == 'E' &&
         header.ident[2] == 'L' && header.ident[3] == 'F' &&
         header.ident[4] == 1 && header.ident[5] == 1 && header.type == 2 &&
         header.machine == 243 &&
         user_memory.contains(header.entry, 1) &&
         header.program_entry_size == sizeof(ProgramHeader) &&
         header.program_offset <= file_size &&
         header.program_count <=
             (file_size - header.program_offset) / sizeof(ProgramHeader);
}

[[nodiscard]] bool read_file(const drivers::fs::root::Node& file, u32 offset,
                             void* output, u32 size) {
  const auto result = drivers::fs::root::read(
      file, offset, reinterpret_cast<u8*>(output), size);
  return result && result.value == size;
}

template <typename Reader>
[[nodiscard]] bool load_image_from(u32 file_size, Reader read, Image& image,
                                   bool writable_only,
                                   u32 preloaded_base) {
  ElfHeader header{};
  if (file_size < sizeof(header) ||
      !read(0, &header, sizeof(header)) ||
      !valid_elf(header, file_size)) {
    return false;
  }

  u32 image_end = user_begin;
  u32 mutable_begin = user_end;
  u32 program_headers = 0;
  // Validate every loadable segment before overwriting the current image.
  for (u32 i = 0; i < header.program_count; ++i) {
    ProgramHeader segment{};
    const u32 program_offset =
        header.program_offset + i * sizeof(ProgramHeader);
    if (!read(program_offset, &segment, sizeof(segment))) {
      return false;
    }
    constexpr u32 interpreter = 3;
    if (segment.type == interpreter) {
      return false;
    }
    if (segment.type != 1) {
      continue;
    }
    if (segment.file_size > segment.memory_size ||
        segment.offset > file_size ||
        segment.file_size > file_size - segment.offset ||
        !user_memory.contains(segment.virtual_address, segment.memory_size)) {
      return false;
    }
    const u32 program_table_size =
        header.program_count * sizeof(ProgramHeader);
    if (header.program_offset >= segment.offset &&
        program_table_size <= segment.file_size &&
        header.program_offset - segment.offset <=
            segment.file_size - program_table_size) {
      program_headers =
          segment.virtual_address + header.program_offset - segment.offset;
    }
    constexpr u32 writable = 2;
    const u32 end = segment.virtual_address + segment.memory_size;
    if (end > image_end) {
      image_end = end;
    }
    if ((segment.flags & writable) != 0 &&
        segment.virtual_address < mutable_begin) {
      mutable_begin = segment.virtual_address;
    }
  }

  if (program_headers == 0) {
    return false;
  }

  for (u32 i = 0; i < header.program_count; ++i) {
    ProgramHeader segment{};
    const u32 program_offset =
        header.program_offset + i * sizeof(ProgramHeader);
    if (!read(program_offset, &segment, sizeof(segment))) {
      return false;
    }
    constexpr u32 load = 1;
    constexpr u32 writable = 2;
    if (segment.type != load ||
        (writable_only && (segment.flags & writable) == 0)) {
      continue;
    }
    auto* destination = reinterpret_cast<u8*>(segment.virtual_address);
    if (preloaded_base != 0) {
      // The interactive kernel ELF contains selected executable file bytes
      // near their linked virtual addresses. Identity-mapped PT_LOAD segments
      // need no simulated SD or CPU copy.
      const u32 source_address = preloaded_base + segment.offset;
      if (segment.virtual_address != source_address) {
        const auto* source = reinterpret_cast<const u8*>(source_address);
        // A linker may place a writable PT_LOAD one page beyond its file
        // offset. Copy only that small non-identity segment, backwards when
        // the preloaded source and destination overlap.
        if (segment.virtual_address > source_address &&
            segment.virtual_address < source_address + segment.file_size) {
          for (u32 byte = segment.file_size; byte != 0; --byte) {
            destination[byte - 1] = source[byte - 1];
          }
        } else {
          memcpy(destination, source, segment.file_size);
        }
      }
    } else {
      if (!read(segment.offset, destination, segment.file_size)) {
        return false;
      }
    }
    memset(destination + segment.file_size, 0,
           segment.memory_size - segment.file_size);
  }
  image = Image{header.entry,
                program_headers,
                header.program_count,
                align_up(image_end, static_cast<u32>(4096)),
                mutable_begin};
  return true;
}

[[nodiscard]] bool load_image(const drivers::fs::root::Node& file,
                              Image& image, bool writable_only = false) {
  if (file.type != drivers::fs::root::Type::regular ||
      file.size > 0xffffffffu) {
    return false;
  }
  const auto reader = [&file](u32 offset, void* output, u32 size) {
    return read_file(file, offset, output, size);
  };
  return load_image_from(static_cast<u32>(file.size), reader, image,
                         writable_only, 0);
}

#ifdef MIKOS_TRIBE_INTERACTIVE
[[nodiscard]] bool load_preloaded_image(const drivers::fs::root::Node& file,
                                        Image& image, usize begin,
                                        usize end) {
  if (file.type != drivers::fs::root::Type::regular || begin > 0xffffffffu ||
      end < begin || file.size != end - begin || file.size > 0xffffffffu) {
    return false;
  }
  const auto reader = [begin](u32 offset, void* output, u32 size) {
    memcpy(output, reinterpret_cast<const void*>(begin + offset), size);
    return true;
  };
  return load_image_from(static_cast<u32>(file.size), reader, image, false,
                         static_cast<u32>(begin));
}

[[nodiscard]] bool load_preloaded_busybox(
    const drivers::fs::root::Node& file, Image& image) {
  return load_preloaded_image(
      file, image, reinterpret_cast<usize>(__embedded_busybox_begin),
      reinterpret_cast<usize>(__embedded_busybox_end));
}

[[nodiscard]] bool load_preloaded_dropbear(
    const drivers::fs::root::Node& file, Image& image) {
  return load_preloaded_image(
      file, image, reinterpret_cast<usize>(__embedded_dropbear_begin),
      reinterpret_cast<usize>(__embedded_dropbear_end));
}
#endif

void copy_path(char* output, const char* input) {
  u32 index = 0;
  while (index != 255 && input[index] != '\0') {
    output[index] = input[index];
    ++index;
  }
  output[index] = '\0';
}

[[nodiscard]] Image required_image(const char* path,
                                   bool writable_only = false,
                                   bool preloaded_busybox = false) {
  const auto node = drivers::fs::root::lookup(path);
  Image image{};
  if (!node ||
#ifdef MIKOS_TRIBE_INTERACTIVE
      (preloaded_busybox ? !load_preloaded_busybox(node.value, image)
                         : !load_image(node.value, image, writable_only))) {
#else
      preloaded_busybox || !load_image(node.value, image, writable_only)) {
#endif
    write_text("MIKOS:BAD_ELF\n");
    shutdown(1);
  }
  return image;
}

[[nodiscard]] u32 copy_string_down(u32 cursor, const char* text) {
  u32 size = 1;
  while (text[size - 1] != '\0') {
    ++size;
  }
  cursor -= size;
  memcpy(reinterpret_cast<void*>(cursor), text, size);
  return cursor;
}

[[nodiscard]] u32 make_initial_stack(const Image& image,
                                     const char* const* arguments,
                                     u32 argument_count) {
  u32 cursor = user_stack_top;
  u32 argument_addresses[16]{};
  if (argument_count == 0 || argument_count > 16) {
    write_text("MIKOS:BAD_ARGUMENTS\n");
    shutdown(6);
  }
  for (u32 i = argument_count; i != 0; --i) {
    cursor = copy_string_down(cursor, arguments[i - 1]);
    argument_addresses[i - 1] = cursor;
  }

  cursor -= 16;
  const u32 random = cursor;
  for (u32 i = 0; i < 16; ++i) {
    reinterpret_cast<u8*>(random)[i] =
        static_cast<u8>(0xa5u ^ static_cast<u8>(i * 29u));
  }

  constexpr u32 null = 0;
  const Aux aux[] = {
      {3, image.program_headers}, {4, sizeof(ProgramHeader)},
      {5, image.program_count},   {6, 4096},
      {7, 0},                     {8, 0},
      {9, image.entry},           {11, 0},
      {12, 0},                    {13, 0},
      {14, 0},                    {16, 0},
      {17, 100},                  {23, 0},
      {25, random},               {31, argument_addresses[0]},
      {0, 0},
  };
  const u32 words =
      1 + argument_count + 1 + 1 + sizeof(aux) / sizeof(aux[0]) * 2;
  const u32 stack = align_down(
      cursor - words * sizeof(u32), static_cast<u32>(16));
  auto* out = reinterpret_cast<u32*>(stack);
  *out++ = argument_count;
  for (u32 i = 0; i < argument_count; ++i) {
    *out++ = argument_addresses[i];
  }
  *out++ = null;
  *out++ = null;
  for (const auto item : aux) {
    *out++ = item.type;
    *out++ = item.value;
  }
  return stack;
}

void disable_interrupt_controllers() {
  auto* machine = reinterpret_cast<volatile u32*>(plic_m_enable);
  auto* supervisor = reinterpret_cast<volatile u32*>(plic_s_enable);
  for (u32 i = 0; i < 3; ++i) {
    machine[i] = 0;
    supervisor[i] = 0;
  }
}

[[nodiscard]] bool flat_and_device_irq_off() {
  u32 mie;
  u32 mstatus;
  u32 satp;
  asm volatile("csrr %0, mie" : "=r"(mie));
  asm volatile("csrr %0, mstatus" : "=r"(mstatus));
  asm volatile("csrr %0, satp" : "=r"(satp));
  const auto* machine = reinterpret_cast<volatile u32*>(plic_m_enable);
  const auto* supervisor = reinterpret_cast<volatile u32*>(plic_s_enable);
  bool plic_off = true;
  for (u32 i = 0; i < 3; ++i) {
    plic_off = plic_off && machine[i] == 0 && supervisor[i] == 0;
  }
  return mie == 0 && (mstatus & 8u) == 0 && satp == 0 && plic_off;
}

#ifndef MIKOS_TRIBE
[[nodiscard]] bool protect_kernel() {
  const u32 lower_top = user_begin >> 2;
  const u32 user_top = user_end >> 2;
  asm volatile("csrw pmpaddr0, %0" : : "r"(lower_top));
  asm volatile("csrw pmpaddr1, %0" : : "r"(user_top));
  constexpr u32 tor_no_access = 0x08;
  constexpr u32 tor_rwx = 0x0f;
  constexpr u32 config = tor_no_access | (tor_rwx << 8);
  asm volatile("csrw pmpcfg0, %0" : : "r"(config));
  u32 actual;
  asm volatile("csrr %0, pmpcfg0" : "=r"(actual));
  return (actual & 0xffffu) == config;
}
#endif

}  // namespace

Process process{};
Scheduler scheduler{};

bool replace_with_executable(TrapFrame& frame, const char* path,
                             const char* const* arguments,
                             u32 argument_count) {
  if (path == nullptr || argument_count == 0 || argument_count > 16) {
    return false;
  }
  const auto node = drivers::fs::root::lookup(path);
  if (!node || node.value.type != drivers::fs::root::Type::regular ||
      (node.value.mode & 0111) == 0) {
    return false;
  }
  const auto busybox_node = drivers::fs::root::lookup("/bin/busybox");
  const bool busybox = busybox_node &&
                       busybox_node.value.inode == node.value.inode;
  Image image{};
#ifdef MIKOS_TRIBE_INTERACTIVE
  const auto dropbear_node =
      drivers::fs::root::lookup("/usr/sbin/dropbear");
  const bool dropbear = dropbear_node &&
                        dropbear_node.value.inode == node.value.inode;
  if (dropbear) {
    write_text("MIKOS:DROPBEAR_PRELOAD_START\n");
  }
  const bool loaded = dropbear ? load_preloaded_dropbear(node.value, image)
                               : load_image(node.value, image, busybox);
  if (dropbear) {
    write_text(loaded ? "MIKOS:DROPBEAR_PRELOAD_OK\n"
                      : "MIKOS:DROPBEAR_PRELOAD_FAIL\n");
  }
  if (!loaded) {
#else
  if (!load_image(node.value, image, busybox)) {
#endif
    return false;
  }
  process.brk = image.brk;
  process.mutable_begin = image.mutable_begin;
  if (busybox) {
    process.mmap_begin = 0x81800000;
  } else {
    constexpr u32 heap_reserve = 512 * 1024;
    if (process.brk > user_stack_top - heap_reserve) {
      return false;
    }
    process.mmap_begin = align_up(process.brk + heap_reserve,
                                  static_cast<u32>(4096));
  }
  process.mmap_cursor = process.mmap_begin;
  const u32 stack = make_initial_stack(image, arguments, argument_count);
  for (auto& value : frame.x) {
    value = 0;
  }
  frame.x[2] = stack;
  frame.mepc = image.entry;
  process.image = busybox ? 0 : 1;
  copy_path(process.executable_path, path);
  process.image_replaced = true;
  return true;
}

void restore_busybox_image() {
  static_cast<void>(required_image("/bin/busybox"));
}

bool restore_executable_image(const char* path) {
  const auto node = drivers::fs::root::lookup(path);
  Image image{};
#ifdef MIKOS_TRIBE_INTERACTIVE
  const auto dropbear_node =
      drivers::fs::root::lookup("/usr/sbin/dropbear");
  if (node && dropbear_node && node.value.inode == dropbear_node.value.inode) {
    return load_preloaded_dropbear(node.value, image);
  }
#endif
  return node && load_image(node.value, image);
}

void start_stress_ng(TrapFrame& frame) {
  constexpr const char* arguments[] = {
      "stress-ng", "--cpu",        "1", "--cpu-method", "loop",
      "--cpu-ops", "4",            "--verify",
      "--metrics-brief",
  };
  if (!replace_with_executable(
          frame, "/bin/stress-ng", arguments,
          sizeof(arguments) / sizeof(arguments[0]))) {
    write_text("MIKOS:STRESS_NG_ARGUMENTS_BAD\n");
    shutdown(6);
  }
}

void uart_put(char value) {
#ifdef MIKOS_TRIBE_INTERACTIVE
  // The interactive UART is attached directly to a host terminal. A line feed
  // alone preserves the cursor column there, so implement the advertised
  // ONLCR behavior and start each new line at column zero.
  if (value == '\n') {
    drivers::uart::put('\r');
  }
#endif
  drivers::uart::put(static_cast<u8>(value));
}

void uart_write(const char* data, u32 size) {
  for (u32 i = 0; i < size; ++i) {
    uart_put(data[i]);
  }
}

void write_text(const char* text) {
  while (*text != '\0') {
    uart_put(*text++);
  }
}

void write_u32(u32 value) {
  char digits[10];
  u32 count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count != 0) {
    uart_put(digits[--count]);
  }
}

[[noreturn]] void shutdown(u32 code) {
  process.exit_code = code;
#ifndef MIKOS_TRIBE
  *reinterpret_cast<volatile u32*>(sifive_test) =
      code == 0 ? 0x5555 : ((code << 16) | 0x3333);
#endif
  for (;;) {
    asm volatile("wfi");
  }
}

}  // namespace mikos

extern "C" void kernel_main() {
  using namespace mikos;
  asm volatile("csrw mie, zero");
  asm volatile("csrw satp, zero");
  asm volatile("csrw mtvec, %0" : : "r"(trap_entry));
  disable_interrupt_controllers();

  static_cast<void>(drivers::uart::initialize());
  write_text("MIKOS:BOOT\n");
  if (!flat_and_device_irq_off()) {
    write_text("MIKOS:PLATFORM_STATE_BAD\n");
    shutdown(3);
  }
  write_text("MIKOS:FLAT_DEVICE_IRQ_OFF\n");
  const bool network_ready = network::initialize();
#ifndef MIKOS_TRIBE_INTERACTIVE
  if (network_ready) {
    static_cast<void>(network::boot_probe());
  }
#else
  if (network_ready) {
    network::Ifreq32 request{};
    network::write_interface_name(request.name);
    network::write_sockaddr_ipv4(
        request.value.address, Ipv4Address{{192, 168, 76, 2}});
    static_cast<void>(network::interface_ioctl(network::siocsifaddr, request));
    network::write_sockaddr_ipv4(
        request.value.address, Ipv4Address{{255, 255, 255, 0}});
    static_cast<void>(
        network::interface_ioctl(network::siocsifnetmask, request));
  }
  write_text("MIKOS:TRIBE_INTERACTIVE\n");
#endif
  if (!drivers::fs::root::initialize()) {
    write_text("MIKOS:EXT4_ROOT_FAIL\n");
    shutdown(8);
  }
  write_text("MIKOS:EXT4_ROOT_OK\n");
  const auto image = required_image(
      "/bin/busybox", false,
#ifdef MIKOS_TRIBE_INTERACTIVE
      true
#else
      false
#endif
  );
  process.brk = image.brk;
  process.mutable_begin = image.mutable_begin;
  process.mmap_begin = 0x81800000;
  process.mmap_cursor = process.mmap_begin;
  process.pid = 1;
  process.parent_pid = 0;
  process.process_group = 1;
  process.session = 1;
  copy_path(process.current_directory, "/");
  copy_path(process.executable_path, "/bin/busybox");
#ifdef MIKOS_TRIBE_INTERACTIVE
  constexpr const char* arguments[] = {
      "busybox", "sh", "-il", "+m"};
#else
  constexpr const char* arguments[] = {
      "busybox", "sh", "-c",
      "echo MIKOS_WRITE_OK >/write-test; "
      "cat /write-test; "
      "mv /write-test /write-moved; "
      "cat /write-moved; "
      "rm /write-moved; "
      "test ! -e /write-moved; "
      "sync; echo MIKOS_BUSYBOX_OK"};
#endif
  const u32 stack = make_initial_stack(
      image, arguments, sizeof(arguments) / sizeof(arguments[0]));
  const u32 probe = process.brk;
  const u32 probe_size =
      static_cast<u32>(uncooperative_probe_end - uncooperative_probe_begin);
  if (!user_memory.contains(probe, probe_size)) {
    write_text("MIKOS:PROBE_REGION_BAD\n");
    shutdown(5);
  }
  memcpy(reinterpret_cast<void*>(probe), uncooperative_probe_begin, probe_size);
  process.brk = align_up(probe + probe_size, static_cast<u32>(4096));

#ifdef MIKOS_TRIBE
  // Minimal Tribe excludes the optional PMP CSR range.
  write_text("MIKOS:PMP_UNAVAILABLE\n");
  write_text("MIKOS:TRIBE_POLLING\n");
#ifdef MIKOS_TRIBE_INTERACTIVE
  arch::start_scheduler_timer();
  write_text("MIKOS:NETWORK_TIMER_ON\n");
#endif
  enter_user(image.entry, stack);
#else
  if (!protect_kernel()) {
    write_text("MIKOS:PMP_BAD\n");
    shutdown(4);
  }
  write_text("MIKOS:PMP_ON\n");
  scheduler.start_next_on_timer(image.entry, stack);
  arch::start_scheduler_timer();
  write_text("MIKOS:SCHED_TIMER_ON\n");
  write_text("MIKOS:UNCOOPERATIVE_ENTRY\n");
  enter_user(probe, stack - 4096);
#endif
}

extern "C" void trap_handler(mikos::TrapFrame* frame) {
  using namespace mikos;
  constexpr u32 user_ecall = 8;
#ifdef MIKOS_TRIBE_INTERACTIVE
  network::poll();
#endif
#ifdef MIKOS_TRIBE
  constexpr u32 illegal_instruction = 2;
  if (frame->mcause == illegal_instruction) {
    const u32 instruction = frame->mtval;
    const u32 opcode = instruction & 0x7fu;
    const u32 function3 = (instruction >> 12) & 7u;
    if (opcode == 0x2fu && function3 == 2u) {
      const u32 destination = (instruction >> 7) & 31u;
      const u32 source1 = (instruction >> 15) & 31u;
      const u32 source2 = (instruction >> 20) & 31u;
      const u32 operation = instruction >> 27;
      const u32 address = frame->x[source1];
      static u32 reservation{};
      static bool reservation_valid{};
      if (user_memory.contains(address, sizeof(u32)) &&
          user_memory.aligned(address, alignof(u32))) {
        auto* memory = reinterpret_cast<volatile u32*>(address);
        const u32 old = *memory;
        u32 result = old;
        bool handled = true;
        bool write = true;
        switch (operation) {
          case 0x02:  // LR.W
            reservation = address;
            reservation_valid = true;
            write = false;
            break;
          case 0x03:  // SC.W
            result = reservation_valid && reservation == address ? 0u : 1u;
            if (result == 0) {
              *memory = frame->x[source2];
            }
            reservation_valid = false;
            write = false;
            break;
          case 0x00:  // AMOADD.W
            result = old + frame->x[source2];
            break;
          case 0x01:  // AMOSWAP.W
            result = frame->x[source2];
            break;
          case 0x04:  // AMOXOR.W
            result = old ^ frame->x[source2];
            break;
          case 0x08:  // AMOOR.W
            result = old | frame->x[source2];
            break;
          case 0x0c:  // AMOAND.W
            result = old & frame->x[source2];
            break;
          case 0x10:  // AMOMIN.W
            result = static_cast<i32>(old) <
                             static_cast<i32>(frame->x[source2])
                         ? old
                         : frame->x[source2];
            break;
          case 0x14:  // AMOMAX.W
            result = static_cast<i32>(old) >
                             static_cast<i32>(frame->x[source2])
                         ? old
                         : frame->x[source2];
            break;
          case 0x18:  // AMOMINU.W
            result = old < frame->x[source2] ? old : frame->x[source2];
            break;
          case 0x1c:  // AMOMAXU.W
            result = old > frame->x[source2] ? old : frame->x[source2];
            break;
          default:
            handled = false;
            break;
        }
        if (handled) {
          if (write) {
            *memory = result;
            reservation_valid = false;
            result = old;
          }
          if (destination != 0) {
            frame->x[destination] = result;
          }
          frame->mepc += 4;
          return;
        }
      }
    }
  }
#endif
  if (frame->mcause == arch::scheduler_timer_cause
#ifdef MIKOS_TRIBE_INTERACTIVE
      || frame->mcause == arch::tribe_user_timer_cause
#endif
  ) {
    arch::rearm_scheduler_timer();
#ifdef MIKOS_TRIBE_INTERACTIVE
    static bool reported_network_timer_tick = false;
    if (!reported_network_timer_tick) {
      reported_network_timer_tick = true;
      write_text("MIKOS:NETWORK_TIMER_TICK\n");
    }
    return;
#else
    u32 mstatus;
    asm volatile("csrr %0, mstatus" : "=r"(mstatus));
    scheduler.on_timer(*frame, (mstatus & 8u) == 0);
    return;
#endif
  }
  if (frame->mcause == user_ecall) {
    frame->x[10] = static_cast<u32>(dispatch_syscall(*frame));
    if (process.image_replaced) {
      process.image_replaced = false;
    } else {
      frame->mepc += 4;
    }
    deliver_pending_signal(*frame);
    return;
  }
  write_text("MIKOS:TRAP cause=");
  write_u32(frame->mcause);
  write_text(" pc=");
  write_u32(frame->mepc);
  write_text(" value=");
  write_u32(frame->mtval);
  write_text("\n");
  shutdown(127);
}
