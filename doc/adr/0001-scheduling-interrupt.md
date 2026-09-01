# ADR-0001: One Scheduling Interrupt

Status: accepted  
Date: 2026-07-25

## Decision

Ordinary device interrupts are forbidden. One architecture-local scheduling
interrupt is permitted, has a minimal bounded handler, and may only cause
user-task preemption.

On RISC-V, an enabled M-mode timer interrupt traps from U-mode into the M-mode
kernel. `MSTATUS.MIE` remains clear while kernel code executes, so kernel work
is not interrupted. The PLIC and all device interrupt sources remain disabled.

On x86, the equivalent mechanism is one local-APIC timer vector with every
other vector and source masked. The timer is an explicitly documented exception
to the otherwise-disabled interrupt controller.

## Reason

A pending flag, queue, or second CPU cannot regain control from an unmodified
CPU-bound user program. Without an asynchronous trap, reliable preemption would
require cooperative calls, loader instrumentation, debug single-stepping, or a
dynamic binary translator. Those choices are either incompatible with ordinary
Linux binaries or substantially more complex than one timer trap.

## Handler contract

The handler may:

- acknowledge or rearm the local timer;
- increment bounded counters;
- save the interrupted user trap frame;
- select a ready task using bounded scheduling policy;
- replace the active trap frame with the selected task's frame.

It may not:

- handle devices or drain general event queues;
- allocate memory;
- format or emit diagnostics;
- block, wait on locks, or invoke a user service;
- preempt kernel code.

The first implementation uses a one-way scheduling decision: a timer preempts
an uncooperative U-mode loop and replaces its trap frame with the initial
BusyBox frame. A later fixed-size task array can use the same mechanism for
round-robin scheduling without changing trap assembly.

## Consequences

- Unmodified CPU-bound applications can be preempted reliably.
- Worst-case scheduling latency is one configured quantum.
- Device processing remains polling-only.
- Timer setup and acknowledgement are architecture-specific; task-frame
  selection remains architecture-neutral.
- Timer-handler cost must be included in the 1% pending/scheduling CPU budget.
