#include <mikos/arch.hpp>
#include <mikos/kernel.hpp>

extern "C" void* memcpy(void*, const void*, mikos::usize);
extern "C" void* memset(void*, int, mikos::usize);
extern "C" void trap_entry();

namespace mikos {
namespace {

inline constexpr u32 uart_base = 0x82000000;
inline constexpr u32 plic_m_enable = 0x0c002000;
inline constexpr u32 plic_s_enable = 0x0c002080;
inline constexpr u32 sifive_test = 0x00100000;

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
};

struct Aux {
  u32 type;
  u32 value;
};

[[nodiscard]] volatile u8& uart_register(u32 offset) {
  return *reinterpret_cast<volatile u8*>(uart_base + offset);
}

[[nodiscard]] bool valid_elf(const ElfHeader& header, u32 blob_size) {
  return header.ident[0] == 0x7f && header.ident[1] == 'E' &&
         header.ident[2] == 'L' && header.ident[3] == 'F' &&
         header.ident[4] == 1 && header.ident[5] == 1 && header.type == 2 &&
         header.machine == 243 &&
         header.program_entry_size == sizeof(ProgramHeader) &&
         header.program_offset <= blob_size &&
         header.program_count <=
             (blob_size - header.program_offset) / sizeof(ProgramHeader);
}

[[nodiscard]] Image load_image(const u8* blob, u32 blob_size) {
  const auto& header = *reinterpret_cast<const ElfHeader*>(blob);
  if (!valid_elf(header, blob_size)) {
    write_text("MIKOS:BAD_ELF\n");
    shutdown(1);
  }

  u32 image_end = user_begin;
  const auto* programs = reinterpret_cast<const ProgramHeader*>(
      blob + header.program_offset);
  for (u32 i = 0; i < header.program_count; ++i) {
    const auto& segment = programs[i];
    if (segment.type != 1) {
      continue;
    }
    if (segment.file_size > segment.memory_size ||
        segment.offset > blob_size ||
        segment.file_size > blob_size - segment.offset ||
        !user_memory.contains(segment.virtual_address, segment.memory_size)) {
      write_text("MIKOS:BAD_SEGMENT\n");
      shutdown(2);
    }
    auto* destination = reinterpret_cast<void*>(segment.virtual_address);
    memcpy(destination, blob + segment.offset, segment.file_size);
    memset(reinterpret_cast<u8*>(destination) + segment.file_size, 0,
           segment.memory_size - segment.file_size);
    const u32 end = segment.virtual_address + segment.memory_size;
    if (end > image_end) {
      image_end = end;
    }
  }

  const u32 program_headers =
      user_begin + header.program_offset;
  return Image{header.entry,
               program_headers,
               header.program_count,
               align_up(image_end, static_cast<u32>(4096))};
}

[[nodiscard]] Image load_busybox() {
  const auto* blob = _binary_busybox_busybox_start;
  return load_image(
      blob, static_cast<u32>(_binary_busybox_busybox_end - blob));
}

[[nodiscard]] Image load_stress_ng() {
  const auto* blob = _binary_stress_ng_source_stress_ng_start;
  return load_image(
      blob,
      static_cast<u32>(_binary_stress_ng_source_stress_ng_end - blob));
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
  u32 argument_addresses[10]{};
  if (argument_count == 0 || argument_count > 10) {
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

}  // namespace

Process process{};
Scheduler scheduler{};

void start_stress_ng(TrapFrame& frame) {
  const auto image = load_stress_ng();
  process.brk = image.brk;
  constexpr u32 heap_reserve = 512 * 1024;
  if (process.brk > user_stack_top - heap_reserve) {
    write_text("MIKOS:STRESS_NG_MEMORY_BAD\n");
    shutdown(7);
  }
  process.mmap_cursor = align_up(process.brk + heap_reserve,
                                 static_cast<u32>(4096));
  constexpr const char* arguments[] = {
      "stress-ng", "--cpu",        "1", "--cpu-method", "loop",
      "--cpu-ops", "4",            "--verify",
      "--metrics-brief",
  };
  const u32 stack = make_initial_stack(
      image, arguments, sizeof(arguments) / sizeof(arguments[0]));
  for (auto& value : frame.x) {
    value = 0;
  }
  frame.x[2] = stack;
  frame.mepc = image.entry;
  process.image = 1;
  process.image_replaced = true;
}

void uart_put(char value) {
  while ((uart_register(5) & 0x20u) == 0) {
  }
  uart_register(0) = static_cast<u8>(value);
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
  *reinterpret_cast<volatile u32*>(sifive_test) =
      code == 0 ? 0x5555 : ((code << 16) | 0x3333);
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

  write_text("MIKOS:BOOT\n");
  if (!flat_and_device_irq_off()) {
    write_text("MIKOS:PLATFORM_STATE_BAD\n");
    shutdown(3);
  }
  write_text("MIKOS:FLAT_DEVICE_IRQ_OFF\n");
  static_cast<void>(network::boot_probe());
  const auto image = load_busybox();
  process.brk = image.brk;
  process.mmap_cursor = 0x81800000;
  constexpr const char* arguments[] = {
      "busybox", "echo", "MIKOS_BUSYBOX_OK"};
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
}

extern "C" void trap_handler(mikos::TrapFrame* frame) {
  using namespace mikos;
  constexpr u32 user_ecall = 8;
  if (frame->mcause == arch::scheduler_timer_cause) {
    arch::rearm_scheduler_timer();
    u32 mstatus;
    asm volatile("csrr %0, mstatus" : "=r"(mstatus));
    scheduler.on_timer(*frame, (mstatus & 8u) == 0);
    return;
  }
  if (frame->mcause == user_ecall) {
    frame->x[10] = static_cast<u32>(dispatch_syscall(*frame));
    if (process.image_replaced) {
      process.image_replaced = false;
    } else {
      frame->mepc += 4;
    }
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
