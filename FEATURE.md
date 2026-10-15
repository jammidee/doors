# DOORS Features

**DOORS** (developed in the jammios repository, formerly MMURTL) is based on **MMURTL V1.0** (Message-based MUltitasking Real-Time kerneL) by
Richard A. Burgess, which is in the public domain. This file lists what the current 32-bit system does and
what the planned 64-bit port adds. The port is described in [docs/PORT64_PLAN.md](docs/PORT64_PLAN.md).

---

## Current features (32-bit, inherited from MMURTL V1.0)

### Kernel
- **Message-based microkernel design.** Tasks talk through *exchanges*, which are mailboxes that queue
  messages and waiting tasks.
- **Messaging primitives:** `SendMsg`, `ISendMsg` (safe from interrupt handlers), `WaitMsg` (blocking),
  `CheckMsg` (non-blocking).
- **Request/response IPC** for system services: `Request`, `Respond`, `MoveRequest`. A request
  block carries a service code, up to three data words and two data buffers (pointer + size).
- **Named system services:** a program registers a service by name (`RegisterSvc` / `UnRegisterSvc`),
  and clients call it by that name. The file system, keyboard and devices are built this way.
- **Preemptive, priority-based multitasking** with 32 priority levels (0–31) and a 10 ms timer tick.
- **Real-time timing:** `Sleep`, `Alarm`/`KillAlarm`, `MicroDelay`, `GetTimerTick`.
- Hardware task switching through 80386 Task State Segments.
- System calls go through 80386 call gates (`CALL FWORD PTR _Name`) at fixed selectors.
- 32-bit error codes on every call (`0` = OK), grouped by subsystem.

### Memory
- 32-bit flat protected mode with paging (4 KB pages).
- **A separate address space for each job**, with the OS mapped into every job.
- Page APIs: `AllocPage`, `AllocOSPage`, `AllocDMAPage` (ISA-DMA-safe memory), `DeAllocPage`,
  `QueryPages`, `GetPhyAdd`.
- **Memory aliasing** between jobs (`AliasMem` / `DeAliasMem`), so services can reach a caller's
  buffers without copying.

### Jobs and programs
- Jobs load from `.RUN` executables (`LoadNewJob`), can `Chain` to another program, and end
  with `ExitJob`/`KillJob`.
- Each job keeps its own command line, path, user name, job name, standard input/output and exit-job settings.
- Jobs can create tasks of their own (`NewTask`, `SpawnTask`).
- `INITIAL.JOB` lists programs to start at boot.
- Fixed limits: 66 tasks, 32 jobs, 256 dynamic exchanges, 512 link blocks, 64 services,
  32 pending timers.

### Devices and drivers
- **A common device driver interface** (`InitDevDr`, `DeviceOp`, `DeviceStat`, `DeviceInit`) for block and
  character devices. Drivers are built into the kernel; they can't be loaded at run time.
- Drivers for the floppy disk, IDE hard disk (PIO), RS-232 serial (150–38400 baud), parallel printer port,
  keyboard and VGA text video.
- ISA DMA setup (`DMASetUp`, `GetDMACount`).
- Interrupt management: `SetIRQVector`, `GetIRQVector`, `MaskIRQ`, `UnMaskIRQ`, `EndOfIRQ`.
- Port I/O (`InByte`…`OutDWord`, `InWords`/`OutWords`), CMOS clock (`GetCMOSTime`, `GetCMOSDate`),
  and the PC speaker (`Beep`, `Tone`).

### File system
- **MS-DOS-compatible FAT file system** (FAT12/16, 8.3 names) running as a system service. It can share
  disks with DOS.
- File API: `CreateFile`, `OpenFile`, `CloseFile`, `ReadBytes`/`WriteBytes`, `ReadBlock`/`WriteBlock`,
  `GetFileSize`/`SetFileSize`, `GetFileLFA`/`SetFileLFA` (seek), `RenameFile`, `DeleteFile`,
  `CreateDir`, `GetDirSector`.

### Video and console
- **Virtual screens for each job.** Each job has its own text screen, and `Ctrl-Alt-PageDown` cycles the
  display and keyboard between jobs. `Ctrl-Alt-Del` kills the job currently on screen.
- Console API: `TTYOut`, `PutVidChars`, `PutVidAttrs`, `GetVidChar`, `ScrollVid`, `ClrScr`,
  `SetXY`/`GetXY`, `EditLine`, `SetVidOwner`/`GetVidOwner`.

### Built-in tools
- **Monitor:** the first job, which shows system status and resource statistics and launches jobs.
- **Debugger:** a built-in debugger with a disassembler. Keys: F1 single step, F2/F3 set/clear breakpoint,
  F4 resume, F5 show exchanges, F6 show tasks, F8 set disassembly address, F9/F10 memory dump in bytes or dwords,
  F12 address info.
- **CLI:** the command shell. Internal commands are `cls`, `copy`, `dir`, `del`, `dump`, `type`, `md`,
  `rd`, `ren`, `path`, `run`, `undel`, `debug`, `monitor`, `exit` and `help`. External commands are configured
  in `COMMANDS.CLI`.
- **Applications:** a text editor, dumb terminal, print utility, screen print, a sample service and client,
  and a memory test.

### Development tools
- **CM32:** a C compiler for MMURTL with a small libc (stdio, stdlib, string, ctype).
- **DASM:** an assembler and linker that builds `.RUN` files from `.ATF` include lists.
- Both compilers also run on MMURTL itself, so the system can rebuild its own programs.
- **fp32:** a real-number math library for CM32 (a later addition).
- DOS-side tools: `MMLOADER` (boots MMURTL from DOS), a boot sector and disk image maker, `DUMPRUN`, `READOBJ`.

### Known limitations
- No DLLs and no dynamically loaded drivers.
- No wildcards in file names. FAT only, with 8.3 names.
- `DeleteDir` isn't finished in the file system.
- No SCSI and no compressed disks. Some newer disk controllers may fail.
- Known bugs are listed in [PROBLEMS.TXT](PROBLEMS.TXT).

---

## Planned features (64-bit port)

Every public API name, parameter order and error code above stays the same. Only pointer and size types widen.

### Platform
- **x86-64 long mode** with a higher-half kernel, booted through Multiboot2/GRUB. It no longer needs DOS.
- 4-level paging and per-job address spaces, so memory isn't limited to 4 GB.
- Built with modern tools: GCC (freestanding C) + NASM + GNU ld. `make run` boots it in QEMU.

### Kernel
- **Software context switching** with FPU/SSE state saving, replacing hardware TSS switching.
- **`SYSCALL`/`SYSRET` system calls** in the same order and numbering as the original call gates.
- Pointer checking on every system call, so a bad pointer returns an error instead of crashing the kernel.
- The same exchange, message, request/response and named-service model, with 32 priorities.
- Programs are ELF64 executables, replacing `.RUN`.

### Storage
- **A modern native file system: ext4** (extents, 64-bit sizes, large volumes, journaling, checksums).
- A **VFS layer** that runs several file systems side by side. **FAT12/16/32 with long names** stays
  for exchanging files.
- The same file API, with **files larger than 4 GB**, long/UTF-8 names, a new `ReadDir` call and a
  finished `DeleteDir`.
- MBR and GPT partitions, a block cache, and virtio-blk or AHCI disk drivers.

### Tools and debugging
- QEMU/GDB debugging from the first boot, then a 64-bit built-in debugger and disassembler.
- The CLI, editor and sample programs ported to 64-bit.

### Optional / later
- 64-bit CM32 and DASM, so the system can rebuild itself again.
- UEFI boot, APIC/HPET timers, SMP (multiple CPUs).
