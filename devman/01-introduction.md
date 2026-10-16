<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 1. Introduction

## What DOORS is

DOORS is a small operating system kernel with the following ideas, all inherited from MMURTL:

- **Message-based.** Tasks cooperate by sending messages to *exchanges* (mailboxes). System services,
  including the file system, are tasks that receive *requests* and send back *responses*. Even
  `Sleep` is a timer message that arrives at the task's own exchange.
- **Multitasking and real-time.** A priority scheduler with 32 levels (0 is the most urgent). A task
  with a strictly higher priority runs as soon as it becomes ready. Tasks of equal priority share
  the CPU in 30 ms slices.
- **Jobs.** A program runs as a *job*: its own address space, one or more tasks, its open files.
  When a job ends, DOORS reclaims everything it owned.
- **Named services.** A task registers an exchange under an 8-character name (`"FILESYS "`,
  `"UPCASE  "`). Clients send requests to the name. Buffers passed in a request are mapped
  (*aliased*) into the service's address space, so a service in another job can work on the
  caller's memory without copying.

## DOORS and MMURTL

MMURTL V1.0 (1991–1999, public domain) is a 32-bit protected-mode OS written for the book
*Developing Your Own 32-bit Operating System*. It ran under DOS-era tools, used hardware task
switching and called the kernel through GDT call gates.

DOORS keeps the **interface** and the **design**, and replaces the implementation:

| | MMURTL V1.0 | DOORS 0.8 |
|---|---|---|
| CPU mode | 32-bit protected mode | x86-64 long mode, higher-half kernel |
| Boot | DOS loader / own boot sector | GRUB (Multiboot2), from CD image or disk |
| Paging | 2-level, 4 GB | 4-level, NX, direct map of all RAM |
| Task switch | hardware TSS (`JMP FAR`) | software context switch, FPU/SSE state saved |
| System calls | call gates `CALL FAR` | `SYSCALL`/`SYSRET`, same numbering |
| Programs | `.RUN` files (DASM) | static ELF64 (GCC) |
| File systems | FAT12/16, 8.3 names | FAT12/16/32 with long names, **ext4** |
| File sizes | 32-bit | 64-bit (files > 4 GB) |
| Bad pointer in a call | could crash the system | returns an error (`ErcBadLinAdd`) |

API names, parameter order and error codes are unchanged. Only types are wider: pointers, sizes,
file positions and messages are 64-bit. Calls that DOORS adds are numbered after MMURTL's last
call (see [chapter 8](08-system-calls.md)).

## Status (1.0.0, milestone M9)

DOORS 1.0.0 passes the same smoke test as MMURTL V1.0 (`docs/SMOKE_TEST.md`, `make smoke`): the
monitor, several CLIs, the editor and the sample programs work as they did in 1994, and the MMURTL
bugs that test found are fixed. Only the debugger row is left for M10.

Finished and covered by automatic tests:

- Booting in long mode, interrupts, timer, keyboard, VGA text console and serial log.
- Physical and virtual memory, per-job address spaces, aliasing between jobs, guard pages.
- Tasks, priorities, time slicing, sleeping.
- All messaging primitives: exchanges, `SendMsg`/`WaitMsg`/`CheckMsg`, `Request`/`Respond`/
  `MoveRequest`, named services, `Alarm`/`KillAlarm`.
- User mode: ELF64 programs in ring 3, a system call layer that checks every pointer, ending
  a faulting job without harming the system, job cleanup without leaks.
- ATA disks, MBR/GPT partitions, a block cache, FAT12/16/32 with long names, and ext4 (read
  and write, with the journal: power-cut safe). Both are reached through the MMURTL file API.

Not yet available (see [chapter 11](11-limits.md)):

- htree directory writes, deeper extent trees on write, and booting from an ext4 system disk (M8b).
  The ext4 journal is done (1.1.0).
- The debugger and the other calls that still return `ErcBadCallGate`.

Done in M9: per-job screens and the keyboard service (0.9.0), the job calls (`Chain`, exit run
file, paths) and a C library (0.9.1), MMURTL's CLI (0.9.2, see [The CLI](03-monitor.md#the-cli)), the
editor and sample programs (0.9.3, see [Programs in C:\DOORS](03-monitor.md#programs-in-cdoors)), and the
64-bit smoke test (1.0.0).
