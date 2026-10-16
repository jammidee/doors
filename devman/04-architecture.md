<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 4. System architecture

## Source tree (`port64/`)

| Path | Contents |
|---|---|
| `boot/boot.asm` | Multiboot2 header, 32-bit entry, the switch to long mode, jump to the kernel |
| `linker.ld` | kernel layout: linked at `0xFFFFFFFF80000000`, loaded at 1 MiB |
| `arch/x86_64/` | GDT/TSS, IDT, PIC, PIT, interrupt stubs, context switch, `SYSCALL` entry, safe memory probes |
| `kernel/` | memory, scheduler, messaging, jobs, system calls, block cache, volumes, FAT, ext4, FILESYS, monitor, self-tests |
| `drivers/` | serial (COM1), VGA text, PS/2 keyboard, ATA disks, CMOS clock |
| `include/doors/` | kernel headers; `erc.h` (error codes), `syscalls.def` (call numbers), `rqblk.h` and `dirent.h` are shared with user programs |
| `user/` | the user library (`lib/`), programs (`progs/`), `INITIAL.JOB` |
| `tools/` | test runner, disk builder, disk checker |

## Boot sequence

1. **GRUB** loads `kernel.elf` and every user program as a Multiboot2 module, then jumps to
   `boot/boot.asm` in 32-bit mode.
2. The boot code builds temporary page tables (the first 1 GiB, mapped at 0 and in the higher half),
   enables PAE and long mode, and jumps to `kmain` in the higher half.
3. `kmain` (`kernel/main.c`):
   - starts the console (VGA and COM1) and reads the Multiboot2 memory map and modules;
   - starts the physical page allocator, then switches to the kernel's own page tables (low identity
     map removed, `.text` read-only and executable, data non-executable, write protection on);
   - sets up the GDT/TSS, IDT, PIC, PIT (100 Hz) and the keyboard;
   - starts the scheduler, messaging, the `SYSCALL` entry, the disks, the volumes and the FILESYS
     service;
   - runs the self-tests if the kernel command line is `test`;
   - loads the programs in `INITIAL.JOB` and enters the [monitor](03-monitor.md).

## Memory

### Virtual address layout

Every address space shares the upper (kernel) half. The lower half belongs to the job.

| Address | Region |
|---|---|
| `0x0000000040000000` | `USER_BASE`: user programs are linked here (as in MMURTL); `AllocPage` memory follows |
| `0x00007FFFFFFF0000` | `USER_STACK_TOP`: user stack, 64 KB, with an unmapped guard page below it |
| `0xFFFF800000000000` | `HHDM_BASE`: all physical RAM, mapped directly |
| `0xFFFFC00000000000` | `OS_BASE`: `AllocOSPage` / `AllocDMAPage` region (512 GiB) |
| `0xFFFFC08000000000` | `STACK_BASE`: kernel stacks, one guarded slot per task (16 KB stack) |
| `0xFFFFC10000000000` | `RQB_UBASE`: read-only window on the request blocks, for user services |
| `0xFFFFFFFF80000000` | the kernel image |

### Physical memory

`kernel/pmm.c` keeps a bitmap of 4 KB pages, built from the firmware memory map. The page
holding the kernel, the modules and the bitmap itself are reserved.

### The memory calls

The MMURTL calls keep their names (`kernel/mem.c`):

- `AllocPage(n, &p)`: `n` pages in the caller's job. `DeAllocPage(p, n)` frees them.
- `AllocOSPage` / `AllocDMAPage`: kernel memory (DMA pages lie below 16 MB). Kernel only.
- `QueryPages(&n)`: free pages in the system.
- `AliasMem(p, cb, job, &alias)`: maps the caller's memory into another job's address space.
  `DeAliasMem` removes the mapping. This is how buffers reach a service in another job.

Paging uses 4-level tables, the NX bit, and an unmapped guard page under every stack. A stack
overflow in a task ends in a page fault, not in silent corruption.

## Tasks and the scheduler

A **task** is a thread of execution: registers, a kernel stack, a priority, an exchange of its
own, and the job it belongs to (`kernel/sched.c`, `include/doors/task.h`).

- **Priorities:** 32 levels, `0` is the most urgent. User tasks default to 25. The FILESYS service
  runs at 5 and the monitor at 20.
- **Preemption:** when a task becomes ready with a strictly higher priority than the running one,
  it runs at once, even from inside an interrupt.
- **Time slicing:** tasks of equal priority take turns every 3 ticks (30 ms), as in MMURTL.
- **Idle:** when nothing is ready, the idle task halts the CPU until the next interrupt.
- **Context switch:** in software (`arch/x86_64/switch.asm`). The FPU/SSE state is saved for every
  task (`FXSAVE`), so user programs may use floating point.
- **Blocking:** `WaitMsg` on an empty exchange, and `Sleep`, take the task off the ready list until
  a message arrives.
- **States** shown by the `tasks` command: `ready`, `running`, `sleeping`, `blocked`, `dead`.

Only one CPU is used. The scheduler is protected by turning interrupts off for short sections.

## Jobs

A **job** is a running program: an address space, its tasks, exchanges, registered services and
open files (`kernel/job.c`).

- Job 1 is the OS and monitor. Job 2 is reserved for the debugger, as in MMURTL. Programs start at
  job 3. Up to 64 jobs exist at a time.
- `LoadNewJob(name)` loads a **static ELF64** program. A name without `:` or `\` is first looked up
  among the boot modules (`hello`), then as a file relative to the caller's path. A path is loaded
  from a volume (`C:\BIN\HELLO.ELF`). Segments are mapped with their own
  permissions: code read-only and executable, data non-executable.
- The first task starts at the program's entry (`_start` in `user/lib/crt0.asm`) in ring 3, with
  a 64 KB stack. When `main` returns, its return value goes to `ExitJob`.
- **Ending a job** (`ExitJob`, `KillJob`, or a CPU fault): the job's tasks stop, its open files
  are closed, its exchanges, services, timers and pending requests are cleaned up, and its memory
  and page tables are freed. The kernel prints `Job number N terminated. Error: E`.
- **Replacing the program** (`Chain`, or `ExitJob` with an exit run file): the same cleanup, but the
  job stays. Its number, screen, keys, command line, path, user name and exit run file are kept, and
  the new program gets a fresh address space and first task (MMURTL kept the JCB the same way).
- **Faults:** a CPU exception in ring 3 (page fault, divide error, general protection, ...) ends
  the job with error `1000 + vector`. For example, a page fault (vector 14) gives `1014`. The
  system keeps running.

## System calls

User programs enter the kernel with the `SYSCALL` instruction (`arch/x86_64/syscall.asm`,
`kernel/syscall.c`):

- `RAX` holds the call number. The numbers come from MMURTL's call gate table
  (`include/doors/syscalls.def`, generated from `OSSOURCE/MPUBLICS.ASM`: number =
  (gate selector − 40h) / 8).
- `RDI` points to an array of arguments, one `U64` each, in MMURTL's parameter order.
- The result (an `erc_t`) comes back in `RAX`.

The user library (`user/lib/libdoors.c`) hides this. Programs call `WaitMsg(...)` like MMURTL C
programs did.

Every pointer a program passes is checked: it must lie in the user half and be mapped with the
needed access. The kernel copies data in and out through routines that turn a fault into an error
code. A bad pointer returns `ErcBadLinAdd` (108) instead of crashing the system.

Calls that are not ported yet, or that only the kernel may use (port I/O, IRQ control,
`AllocOSPage`, `ISendMsg`, `NewTask`, ...), return `ErcBadCallGate` (49), which is MMURTL's answer
for an empty call gate.

## Interrupts and devices

| Vector | Source |
|---|---|
| 0–31 | CPU exceptions. Double fault and NMI run on their own stacks (IST) |
| 32 (IRQ 0) | PIT timer, 100 Hz: ticks, sleeping tasks, time slices |
| 33 (IRQ 1) | PS/2 keyboard: each key is translated to a MMURTL key code and sent (`ISendMsg`) to the keyboard service task, which handles Ctrl-Alt keys and queues the rest for the job on screen |
| 32–47 | the remaining legacy PIC interrupts |

## Screens and keyboard

Every job has an 80×25 virtual screen (`kernel/video.c`, as MMURTL's `VIDEO.ASM`). Writes go into
the job's own buffer, and also into VGA memory if the job is the **video owner**. `SetVidOwner` copies
the new owner's buffer to the screen. Job 1's screen is the kernel console (`kprintf`), with a status
line in row 0. A panic always switches to job 1's screen.

The keyboard service (`kernel/kbdsvc.c`, as `KEYBOARD.ASM`) is a kernel task at priority 1. It puts
each key into a 64-key buffer of the job that owns the screen. `ReadKbd` takes keys from the caller's
own job's buffer, so a program only gets keys typed while its screen was shown. Ctrl+Alt keys are
global: Ctrl-Alt-PgDn shows the next job, Ctrl-Alt-Del kills the job on screen.

## Disks

The ATA driver (`drivers/ata.c`) uses PIO with LBA48 addressing. Disks are device numbers 12–15
(primary/secondary, master/slave), as MMURTL numbered its hard disks. Sectors pass through a
write-through block cache of 1024 × 512 bytes (`kernel/bcache.c`).
