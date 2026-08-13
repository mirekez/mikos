OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;
  __kernel_begin = .;

  .text : ALIGN(16) {
    KEEP(*(.text.boot))
    *(.text .text.*)
  }

  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16) {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  .bss (NOLOAD) : ALIGN(16) {
    __bss_begin = .;
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Large syscall scratch storage does not need boot-time zeroing. */
  .noinit (NOLOAD) : ALIGN(16) {
    *(.noinit .noinit.*)
  }

  . = ALIGN(16);
  __kernel_stack_bottom = .;
  . += 64K;
  __kernel_stack_top = .;
  __kernel_end = .;

  /*
   * Interactive Tribe kernels preload the BusyBox file at the same address
   * as its first PT_LOAD segment. The section is empty for all other kernels.
   * Keeping it out of __kernel_end preserves the kernel/user boundary check.
   */
  .user_busybox 0x81000000 : ALIGN(16) {
    __embedded_busybox_begin = .;
    KEEP(*(.user_busybox))
    __embedded_busybox_end = .;
  }

  .user_dropbear 0x81400000 : ALIGN(16) {
    __embedded_dropbear_begin = .;
    KEEP(*(.user_dropbear))
    __embedded_dropbear_end = .;
  }

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame*)
    *(.riscv.attributes)
  }

  ASSERT(__kernel_end < 0x81000000, "kernel overlaps user image")
}
