.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
  csrw mie, zero
  li t0, 8
  csrc mstatus, t0

#ifdef MIKOS_TRIBE
  /* MikOS currently schedules on the boot hart. Keep additional Tribe harts
     from racing through the shared BSS, stack, and device initialization. */
  /* cpphdl supplies the hart ID in the standard reset argument a0; its
     mhartid CSR is currently hardwired to zero. */
  bnez a0, .Lsecondary_hart
#endif

  la sp, __kernel_stack_top

  la t0, __bss_begin
  la t1, __bss_end
1:
  bgeu t0, t1, 2f
  sw zero, 0(t0)
  addi t0, t0, 4
  j 1b
2:
  call kernel_main
3:
  wfi
  j 3b
#ifdef MIKOS_TRIBE
.Lsecondary_hart:
  wfi
  j .Lsecondary_hart
#endif
.size _start, . - _start

.section .text, "ax"
.global enter_user
.type enter_user, @function
enter_user:
  csrw mepc, a0
  la t0, __kernel_stack_top
  csrw mscratch, t0

  csrr t0, mstatus
  li t1, ~(3 << 11)
  and t0, t0, t1
#ifdef MIKOS_TRIBE_INTERACTIVE
  li t1, (1 << 7)
  or t0, t0, t1
#else
  li t1, ~(1 << 7)
  and t0, t0, t1
#endif
  li t1, ~(1 << 3)
  and t0, t0, t1
  csrw mstatus, t0

  mv sp, a1
  li ra, 0
  li gp, 0
  li tp, 0
  li s0, 0
  li s1, 0
  li s2, 0
  li s3, 0
  li s4, 0
  li s5, 0
  li s6, 0
  li s7, 0
  li s8, 0
  li s9, 0
  li s10, 0
  li s11, 0
  li a0, 0
  li a1, 0
  li a2, 0
  li a3, 0
  li a4, 0
  li a5, 0
  li a6, 0
  li a7, 0
  mret
.size enter_user, . - enter_user

.section .rodata.user_probe, "a"
.balign 4
.global uncooperative_probe_begin
.global uncooperative_probe_end
uncooperative_probe_begin:
  addi t0, t0, 1
  j uncooperative_probe_begin
uncooperative_probe_end:
