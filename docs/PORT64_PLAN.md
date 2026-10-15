# MMURTL x86-64 Port Plan

Goal: a 64-bit (long mode) MMURTL that keeps the original architecture — message-based kernel
with exchanges, `Request`/`Respond`, named system services, jobs with private address
spaces — and runs on QEMU first, then real hardware.

This is a re-implementation guided by the original source, not a mechanical conversion.
Long mode removes hardware task switching and largely ignores segmentation, and the original
toolchain (CM32, DASM) can't produce 64-bit code.

## Guiding decisions

| Decision | Choice | Why |
|---|---|---|
| Toolchain | `x86_64-elf-gcc` (freestanding C11) + NASM + GNU ld | CM32/DASM are 32-bit only. Extending them is a project of its own and can be done later (Phase 9). |
| Boot | Multiboot2 via GRUB (QEMU `-cdrom`); 32→64 trampoline in the kernel | Removes the DOS/`MMLOADER` dependency, and the same image works on real hardware. UEFI can come later. |
| Context switch | Software: save callee-saved regs + RSP in a TCB, swap stacks | Long mode has no hardware task switch. |
| TCB | New `struct task`, separate from the single CPU TSS | The 64-bit TSS only holds RSP0/IST. The OS fields from `TSS.INC` move to the TCB. |
| System calls | `SYSCALL`/`SYSRET`, call number in RAX, args in registers (SysV order) | Fast and standard. A call number table replaces the gate selectors (keep the same numbering: gate `40h` → call 0, `48h` → 1, …). |
| Paging | 4-level, higher-half kernel, per-job PML4 | Kernel mapped into every job (like the original OS-in-every-address-space design). |
| Calling convention | System V AMD64 everywhere | Drops Pascal ordering and callee-pops. |
| Data model | LP64. MMURTL `long` (32-bit) API params become `int32_t`/`uint32_t`; pointers and sizes become 64-bit | Keeps error codes and exchange numbers 32-bit. Pointers widen. |
| Source tree | New `port64/`; original tree stays untouched as reference | Lets the two be compared side by side. |
| API compatibility | **Decided:** keep every public API name, parameter order and `Erc*` error code. Only types widen (pointers and sizes to 64-bit). | Original apps and the book stay valid references. Ported samples need type fixes only. |
| File system | **Decided:** a modern native FS behind a VFS layer. Recommended format: **ext4** (subset). FAT stays read/write for interchange only. | ext4 gives long names, 64-bit sizes, large volumes, extents and journaling. Host tooling (`mkfs.ext4`, `debugfs`, `e2fsck` in WSL) can build and verify images. |

## Target layout

```
port64/
  Makefile            # builds kernel.elf, mmurtl.iso; `make run` launches QEMU
  linker.ld
  boot/               # multiboot2 header, 32-bit entry, long-mode trampoline (NASM)
  arch/x86_64/        # gdt.c idt.c isr.asm pic.c pit.c context.asm syscall.asm paging.c
  kernel/             # exch.c msg.c rqb.c task.c sched.c job.c svc.c timer.c mem.c
  drivers/            # video.c keyboard.c floppy.c ide.c rs232.c parallel.c dma.c
  services/           # fsys/ (vfs.c, fat.c, ext4.c, jbd2.c), monitor.c, debugger.c
  block/              # block cache, MBR/GPT partitions
  include/mmurtl/     # public API headers (from MKERNEL.H, MJOB.H, MFILES.H, ...)
  libc/               # ported CM32/LIB (stdio, string, stdlib, ctype)
  user/               # crt0, syscall stubs, ported sample apps (CLI, EDIT, ...)
  tools/              # mkfs / image builder, run-file loader replacement (ELF)
```

## Phases

Each phase ends with something that boots and can be checked in QEMU. Tick items as they land.

### Phase 0 — Baseline and reference (32-bit original)
- [x] `git init` and commit the pristine tree (`d775b74`).
- [x] Boot the original in an emulator: `tools/boot32/run.sh` (QEMU, floppy A: + FAT16 IDE C:).
      The Monitor, CLI and `dir` on A:/C: work. See `devlogs/2026-10-15.md`.
- [ ] Write a smoke-test checklist (`docs/SMOKE_TEST.md`) to use as the acceptance test for the port.
- [x] Install the toolchain in WSL Ubuntu 22.04: gcc 11.4 (freestanding), nasm, qemu, grub-mkrescue, xorriso,
      mtools, e2fsprogs, gdb.

### Phase 1 — Long-mode boot
- [ ] Multiboot2 header and 32-bit entry, then build temporary page tables (identity + higher half).
      Set CR4.PAE, EFER.LME, CR0.PG, and do a far jump to 64-bit code.
- [ ] 64-bit GDT: null, kernel code/data, user data/code (ordered for SYSRET), TSS.
- [ ] VGA text console (port `VIDEO.ASM`: `TTYOut`, `PutVidChars`, cursor).
- **Done when:** QEMU shows "MMURTL/64" banner.

### Phase 2 — Interrupts and time
- [ ] 64-bit IDT. Port the exception handlers from `EXCEPT.ASM`, using IST stacks for #DF/#NMI.
- [ ] Remap the 8259 PIC and add the PIT timer at the original tick rate (`TMRCODE.ASM`); `Sleep`/`Alarm` come later.
- [ ] Keyboard ISR (`KEYBOARD.ASM`) into a ring buffer.
- **Done when:** the tick counter and typed keys echo on screen, and a deliberate #PF prints a register dump.

### Phase 3 — Memory management (port `MEMCODE.ASM`)
- [ ] Physical page allocator from the Multiboot2 memory map, replacing the original's PAM bitmap
      (`sPAM`, `_nPagesFree`). The bitmap idea can stay.
- [ ] 4-level paging API: `map`, `unmap`, `LinToPhy`, per-job PML4 creation with the kernel half shared.
- [ ] Re-implement `AllocOSPage`, `AllocPage`, `AllocDMAPage` (below 16 MB for ISA DMA),
      `DeAllocPage`, `QueryPages`, `GetPhyAdd`, `AliasMem`, `DeAliasMem`. Keep the "alias" AVL bit.
- [ ] Kernel heap for TCBs, exchanges, link blocks and RQBs. The original uses fixed pools
      (`nTSS`, `nLB`, `nExch`); keep pools first and add a heap later.
- **Done when:** an allocate/free/alias self-test passes at boot.

### Phase 4 — Tasks and scheduler (port `KERNEL.ASM` scheduling half)
- [ ] `struct task` holding the TSS.INC OS fields: exchange, `pJCB`, Tid, priority (0–31), message block,
      `pLBRet`, next pointer, saved RSP, CR3, debug flag.
- [ ] `context.asm`: `switch_to(prev, next)`, saving and restoring callee-saved regs, RSP, CR3 (if changed)
      and the TSS.RSP0 update. FPU/SSE state uses `FXSAVE`, eager at first.
- [ ] Ready queue with 32 priorities: `enQueueRdy`, `deQueueRdy`, `ChkRdyQ`, preemption on the timer tick
      with the same policy as the original.
- [ ] `NewTask`, `SpawnTask`, `SetPriority`, `GetPriority`, `GetTSSExch`.
- **Done when:** two kernel tasks of different priority interleave output, and preemption works.

### Phase 5 — Messaging and IPC (port `KERNEL.ASM` messaging half, `RQBCODE.ASM`, `SVCCODE.ASM`)
- [ ] Exchanges and link blocks: `AllocExch`, `DeAllocExch`, `SendMsg`, `ISendMsg`, `WaitMsg`, `CheckMsg`.
- [ ] Request blocks: `Request`, `Respond`, `MoveRequest`, `SendAbort`. Widen pointer and size fields
      in `RQB.INC` to 64-bit, and keep the codes and exchanges 32-bit.
- [ ] Service registry: `RegisterSvc`, name lookup (`GetExchange`).
- [ ] Fix the original `__Request` bug where `dData2` is read from `[EBP+20]` (same as `dData0`),
      `OSSOURCE/KERNEL.ASM:546`. Confirm it against the book before treating it as a bug.
- **Done when:** a test service answers `Request`s from a client task, and timeouts/aborts behave.

### Phase 6 — Ring 3, system calls, jobs (port `JOBC.C`, `JOBCODE.ASM`, `MPUBLICS.ASM`)
- [ ] `SYSCALL`/`SYSRET` entry: MSR setup (STAR, LSTAR, FMASK), swap to the kernel stack, dispatch table.
      Validate user pointers (canonical, below the user limit).
- [ ] Generate `include/mmurtl/syscalls.h` and user stubs from one table that mirrors `MPUBLICS.ASM` order.
- [ ] Executable format: ELF64 static, replacing the `.RUN` format (`RUNFILE.H`). The loader maps
      segments into a new job's PML4 and sets up the stack and job control block (`JOB.INC`).
- [ ] Job management: `LoadNewJob`, `ExitJob`, `Chain`, `KillJob`, job control blocks, `INITIAL.JOB` processing.
- **Done when:** a ring-3 "hello" ELF runs as a job and exits cleanly. A bad pointer passed to a call
  returns an error instead of crashing the kernel.

### Phase 7 — Drivers and file system (C sources port fairly directly)
- [ ] Device driver interface (`DEVDRVR.ASM`, `MDEVDRV.H`): `InitDevDr`, `DeviceOp`, `DeviceStat`, `DeviceInit`.
- [ ] ISA DMA (`DMACODE.ASM`), floppy (`FLOPPY.C`), IDE PIO (`HARDIDE.C`), RS232, parallel.
      Mostly type fixes (`long` vs pointer, `far`), plus port I/O through inline-asm helpers.
- [ ] Block cache plus partition table (MBR and GPT) under the file system.
- [ ] Virtio-blk or AHCI driver. A modern FS implies large disks, and PIO IDE stays only for compatibility.
- **Done when:** sectors on a GPT-partitioned QEMU disk can be read and written through the block cache.

### Phase 7b — File system
The file system service keeps the original MMURTL API from `MFILES.H` with the same names and parameter order:
`CreateFile`, `CreateDir`, `DeleteDir`, `OpenFile`, `RenameFile`, `GetFileSize`, `SetFileSize`,
`ReadBlock`, `WriteBlock`, `ReadBytes`, `WriteBytes`, `GetFileLFA`, `SetFileLFA`, `CloseFile`, `DeleteFile`.
- Names are already passed as `(pName, cbName)`, so long names and UTF-8 need no API change.
  Raise the internal path limit (for example to 4096).
- File sizes and positions widen to 64-bit: `dSize`, `pdSizeRet`, `dLFA`, `pdLFARet`, `nBytes`
  become `int64_t`/`uint64_t`. This is the only signature change.
- `GetDirSector` is FAT-specific. Keep it for compatibility on FAT volumes. On ext4 it returns entries in a
  generic record format. Add `ReadDir(handle, pEntriesRet, cbMax, pcbRet)` as a new call at the end of the
  syscall table (new calls go after the original ones, so numbering is unchanged).
- `DeleteDir`, which the original never finished (`PROBLEMS.TXT` 2a), gets implemented.

Steps:
- [ ] VFS layer: mount table, path resolution, vnode/inode cache, open-file table per job. `FSYS` service
      becomes the VFS front end and still runs as a registered service at the same service name.
- [ ] FAT driver: port `FSYS.C` (FAT12/16), add FAT32 and VFAT long names. Used for floppies, USB images and
      data exchange with the host. Consider the "updated filesystem" mentioned in `PROBLEMS.TXT` item 4.
- [ ] ext4 read-only: superblock, block group descriptors (64-bit), inodes, extent trees, linear and
      htree directories, checksums (metadata_csum) verified on read.
- [ ] ext4 read/write: block and inode allocation, extent insert/split, directory insert/delete, rename,
      truncate. No journal yet; mount refuses if `needs_recovery` is set.
- [ ] Journaling (jbd2): replay on mount, then ordered-mode metadata journaling for writes.
- [ ] Image tool (`tools/`): build an ext4 system disk holding `MMSYS` contents (WSL `mkfs.ext4 -d`),
      plus a FAT image for interchange.
- [ ] Test suite: run fsstress-style random create/write/rename/delete from a test job, then `e2fsck -fn`
      the image on the host. It must come back clean.
- Unsupported features (inline data, encryption, compression and similar) refuse a rw mount and allow ro
      where safe. Mount checks incompat flags.
- **Done when:** MMURTL boots from an ext4 root, the CLI copies/renames/deletes files with long names,
  files larger than 4 GB work, and `e2fsck` reports the image clean after the stress test.

### Phase 8 — User land
- [ ] Port the CM32 libc (`CM32/LIB`) to `port64/libc`, or use a small existing one.
      Port `fp32` if wanted (fix its hardcoded `c:\cm32\inc` includes).
- [ ] Monitor (`MONITOR.C`), CLI (`MSAMPLES/CLI`), editor, the other samples.
- [ ] Debugger (`DEBUGGER.ASM`, `UASM.C`): rewrite for 64-bit, with an x86-64 disassembler
      (replace UASM, or embed a small one). Lowest priority. Until then, use the QEMU gdbstub (`-s -S`).
- **Done when:** everything in `docs/SMOKE_TEST.md` passes on the 64-bit build.

### Phase 9 — Optional
- [ ] 64-bit backend for CM32 and an x86-64 DASM, so the system can rebuild itself the way the original could.
- [ ] UEFI boot, APIC/HPET instead of PIC/PIT, AHCI instead of PIO IDE, SMP.

## Risks and open questions

- **Original semantics live in assembly.** Kernel primitives are hand-written ASM with
  register-level contracts (for example `GetExchange` returns in ESI). Port the behavior, not the
  instructions, and write a short test for each primitive before relying on it.
- **Interrupt-time sends.** `ISendMsg` runs with interrupts off. Keep the same rule in the new kernel:
  no allocation and no blocking.
- **Struct layouts.** `TSS.INC`, `JOB.INC`, `RQB.INC` and `RUNFILE.H` use fixed offsets. Rebuild them as C
  structs with `_Static_assert` on sizes, and don't carry the numeric offsets over.
- **Known original bugs** (`PROBLEMS.TXT`): a double fault under filesystem load plus a CLI launch, and
  Ctrl-Alt-Del breaking task switching. Watch for them reappearing. They may be design-level.
- **ISA DMA** needs pages below 16 MB. Make sure the physical allocator supports a low zone.
- **ext4 write path is the largest single piece of work** (extent splits, allocation, jbd2). Ship read-only first,
  and keep writes behind a mount flag until the `e2fsck` stress test passes repeatedly.
- **New error codes** for the VFS/ext4 (for example "unsupported feature" or "needs recovery") go above the existing
  file system `Erc*` range in `MOSEDF.INC`. Existing codes keep their meaning.
- **Open:** confirm ext4 as the native format. Alternatives are ext2 (much simpler, but no extents or journal) or a
  custom MMURTL format (no host tooling).

## Module mapping (original → port64)

| Original | Port target | Notes |
|---|---|---|
| MOSIDT / EXCEPT / INTCODE | arch/x86_64/idt.c, isr.asm | rewrite |
| MOSGDT | arch/x86_64/gdt.c | rewrite; no call gates |
| MOSPDR / MEMCODE | arch/x86_64/paging.c, kernel/mem.c | rewrite (4-level) |
| MPUBLICS | kernel/syscall table + user stubs | same order/numbering |
| MAIN / INITCODE | boot/, kernel/init.c | rewrite |
| KERNEL | kernel/sched.c, exch.c, msg.c, context.asm | re-implement |
| RQBCODE / SVCCODE | kernel/rqb.c, svc.c | re-implement |
| TMRCODE | arch/x86_64/pit.c, kernel/timer.c | re-implement |
| JOBCODE / JOBC.C | kernel/job.c, ELF loader | port + new loader |
| VIDEO / KEYBOARD | drivers/ | rewrite (small) |
| DEVDRVR / DMACODE | drivers/devdrvr.c, dma.c | rewrite |
| FLOPPY.C / HARDIDE.C / RS232.C / PARALLEL.C | drivers/ | port (type fixes) |
| FSYS.C | services/fsys/vfs.c, fat.c (+ new ext4.c, jbd2.c) | FAT ported; VFS and ext4 new; API kept |
| MFILES.H | include/mmurtl/files.h | same names; sizes/offsets 64-bit; `ReadDir` added |
| MONITOR.C | services/monitor.c | port |
| DEBUGGER / UASM.C | services/debugger.c | rewrite last |
| NUMCNVRT / MISCCODE | libc / kernel/util.c | rewrite in C |
| CM32 / DASM / MSCODE16 | — | replaced by GCC/NASM/GRUB; kept as reference |
