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

  . = ALIGN(16);
  __kernel_stack_bottom = .;
  . += 64K;
  __kernel_stack_top = .;
  __kernel_end = .;

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame*)
    *(.riscv.attributes)
  }

  ASSERT(__kernel_end < 0x81000000, "kernel overlaps user image")
}
