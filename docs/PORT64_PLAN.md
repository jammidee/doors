# DOORS x86-64 Port Plan

Goal: a 64-bit (long mode) DOORS (formerly MMURTL) that keeps the original MMURTL architecture — message-based kernel
with exchanges, `Request`/`Respond`, named system services, jobs with private address
spaces — and runs on QEMU first, then real hardware.

This is a re-implementation guided by the original source, not a mechanical conversion.
Long mode removes hardware task switching and largely ignores segmentation, and the original
toolchain (CM32, DASM) can't produce 64-bit code.

## Guiding decisions

| Decision | Choice | Why |
|---|---|---|
| Toolchain | Host `gcc` (x86-64, freestanding C11, `-mcmodel=kernel -mno-red-zone`) + NASM + GNU ld | CM32/DASM are 32-bit only. Extending them is a project of its own and can be done later (Phase 9). |
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
  include/doors/     # public API headers (from MKERNEL.H, MJOB.H, MFILES.H, ...)
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
- [x] Write a smoke-test checklist (`docs/SMOKE_TEST.md`) to use as the acceptance test for the port. All rows
      were run on 32-bit on 2026-10-16. Bugs found are listed there.
- [x] Script the rows: `tools/boot32/smoke.py` (PASS 20, XFAIL 3 on 32-bit). Port it to drive the 64-bit
      build at M9.
- [x] Install the toolchain in WSL Ubuntu 22.04: gcc 11.4 (freestanding), nasm, qemu, grub-mkrescue, xorriso,
      mtools, e2fsprogs, gdb.

### Phase 1 — Long-mode boot
- [x] Multiboot2 header and 32-bit entry, then build temporary page tables (identity + higher half).
      Set CR4.PAE, EFER.LME, CR0.PG, and do a far jump to 64-bit code. (`port64/boot/boot.asm`: first 1 GiB
      mapped at 0 and at 0xFFFFFFFF80000000 with 2 MiB pages.)
- [x] 64-bit GDT: null, kernel code/data, user data/code (ordered for SYSRET), TSS. Moved to Phase 2 and
      done there (`arch/x86_64/gdt.c`); M1 used a minimal boot GDT.
- [x] VGA text console (TTY output, scrolling, cursor) plus COM1 serial log and `kprintf`.
- [x] Multiboot2 info: loader name, command line, memory map.
- [x] `make run` / `make debug` / `make test` (headless, serial log + isa-debug-exit).
- **Done when:** QEMU shows the "DOORS" 64-bit banner. ✅ 2026-10-16 (`make test`: `DOORS: M1 OK`).

### Phase 2 — Interrupts and time
- [x] 64-bit IDT (256 vectors, `arch/x86_64/idt.c` + `isr.asm` stubs) with exception register dumps;
      IST stacks for #DF (IST1) and NMI/#MC (IST2). GDT with user segments ordered for SYSRET, and a TSS.
- [x] Remap the 8259 PIC (0x20-0x2F, spurious IRQ 7/15 handled) and add the PIT at 100 Hz (10 ms, as
      `TMRCODE.ASM`). `Sleep`/`Alarm` come with tasks (Phase 4).
- [x] Keyboard ISR (scan set 1, Shift/Caps) into a ring buffer.
- [x] Fixed status bar (row 0) with the live tick count, like MMURTL's Monitor.
- [x] `make test` driven by `tools/qemu_test.py` (types into the guest via the QEMU monitor).
- **Done when:** the tick counter and typed keys echo on screen, and a deliberate #PF prints a register dump.
  ✅ 2026-10-16: 100 ticks per RTC second; int3 handled and resumed; "doors" typed and read back;
  #PF dump with CR2=dead0000 err=2.

### Phase 3 — Memory management (port `MEMCODE.ASM`)
- [x] Physical page allocator from the Multiboot2 memory map (`kernel/pmm.c`): a bitmap like MMURTL's PAM,
      covering all RAM; ordinary allocations avoid the < 16 MB DMA zone; pages are zeroed.
- [x] 4-level paging (`kernel/vmm.c`): own kernel tables (boot identity map dropped), direct map of all
      RAM at `HHDM_BASE`, kernel image with section permissions (.text RX, .rodata R, rest RW+NX),
      EFER.NXE + CR0.WP, guard page under the kernel stack; map/unmap/translate/find_free;
      per-job PML4 with PML4[256..511] shared (all kernel PDPTs created up front).
- [x] `AllocOSPage`, `AllocPage`, `AllocDMAPage` (contiguous, < 16 MB), `DeAllocPage`, `QueryPages`,
      `GetPhyAdd`, `AliasMem`, `DeAliasMem` (`kernel/mem.c`) with MMURTL's names, parameters and error
      codes; the alias AVL bit (bit 9) is kept.
- [x] Fault-safe `probe_read`/`probe_write` with a fixup table (basis for user-pointer checks in M6).
- [ ] Kernel heap for TCBs, exchanges, link blocks and RQBs. **Moved to Phase 4**, where those objects
      appear; start with fixed pools as MMURTL did (`nTSS`, `nLB`, `nExch`), carved from `AllocOSPage`.
- [ ] Locking around the allocators (MMURTL serialised memory calls through an exchange) — Phase 4.
- **Done when:** an allocate/free/alias self-test passes at boot. ✅ 2026-10-16 (`make test`, 7 M3 checks,
  including a leak check over two address spaces and a stack overflow caught as #DF on IST1).

### Phase 4 — Tasks and scheduler (port `KERNEL.ASM` scheduling half)
- [x] `struct task` (`include/doors/task.h`) with the TSS.INC OS fields: Tid, job, priority (0–31), exchange,
      debug flag, next link, saved RSP, address space; plus a guarded 16 KB kernel stack per task in its own
      region (`STACK_BASE`). The message block and `pLBRet` come with messaging (Phase 5).
- [x] `arch/x86_64/switch.asm`: `switch_to` saves/restores callee-saved regs and RSP; CR3 is changed if the
      address space differs, and TSS.RSP0 is updated. FPU/SSE saving is not needed yet (kernel is
      `-mgeneral-regs-only`); **add `FXSAVE` with user mode (Phase 6)**.
- [x] Ready queue with 32 priorities (0 highest) and MMURTL's timer policy: switch to an equal-or-higher
      priority ready task after a 3-tick (30 ms) slice. Deliberate change: a task with a strictly higher
      priority that becomes ready (woken or spawned) runs at once.
- [x] `NewTask` (kernel code segment only until M6), `SpawnTask` (`fOSCode` only until M6), `SetPriority`,
      `GetPriority`, `Sleep` (sleep list in the timer; becomes timer blocks + exchanges in Phase 5),
      `GetTimerTick`. Idle task, exited tasks reaped (stacks freed). `GetTSSExch` → Phase 5.
- [x] Interrupt-off locking around the page allocator and the MMURTL memory calls (moved from Phase 3).
- **Done when:** two kernel tasks of different priority interleave output, and preemption works.
  ✅ 2026-10-16: Sleep(50) back after exactly 50 ticks while two tasks spin; fair round robin (17 slices in
  50 ticks); 34 switches with 19k register-heavy rounds all correct; stacks reclaimed.

### Phase 5 — Messaging and IPC (port `KERNEL.ASM` messaging half, `RQBCODE.ASM`, `SVCCODE.ASM`)
- [x] Exchanges and link blocks (`kernel/msg.c`): `AllocExch`, `DeAllocExch`, `GetTSSExch`, `SendMsg`,
      `ISendMsg`, `WaitMsg`, `CheckMsg`. Messages are two U64 (MMURTL: two DWORDs). Pools are carved from
      AllocOSPage: 1024 exchanges, 4096 link blocks.
- [x] Request blocks (`kernel/rqb.c`): `Request`, `Respond`, `MoveRequest`; `struct RqBlk` mirrors RQB.INC
      with pointers and sizes widened (codes, exchanges, dData0-2 stay 32-bit). The service's
      WaitMsg/CheckMsg aliases pData1/pData2 into its job, and Respond de-aliases (as MMURTL).
      `SendAbort` → Phase 6 (needs job teardown).
- [x] Service registry: `RegisterSvc`, `UnRegisterSvc`, name lookup (8-byte, case sensitive).
- [x] Timer blocks: `Alarm`, `KillAlarm`, and `Sleep` rebuilt on them (a message to the task's own
      exchange), replacing the M4 sleep list.
- [x] The `__Request` `dData2` bug: MMURTL's own header documents dData2 at `[EBP+12]` but the code reads
      `[EBP+20]` (dData0's slot), so services got dData0 twice. Fixed; the self-test checks 7,8,9 arrive intact.
- [x] Keyboard keys delivered as messages (ISendMsg from the IRQ); the interactive monitor blocks in WaitMsg.
- **Done when:** a test service answers `Request`s from a client task, and timeouts/aborts behave.
  ✅ 2026-10-16: NUMBERS service in the same job; UPCASE service in job 3 answering a job 2 client through
  aliased buffers and a MoveRequest to a worker; Alarm timeouts; ErcOwnerAbort path; no leaks.

### Phase 6 — Ring 3, system calls, jobs (port `JOBC.C`, `JOBCODE.ASM`, `MPUBLICS.ASM`)
- [x] `SYSCALL`/`SYSRET` (`arch/x86_64/syscall.asm`, `kernel/syscall.c`): STAR/LSTAR/FMASK, switch to the task's
      kernel stack, dispatch. Args are passed as a user array of U64 (Request has 12). User pointers are
      range-checked and copied with fault fixups (`uaccess.asm`); kernel-only and unported calls return
      `ErcBadCallGate` (49).
- [x] One call table: `include/doors/syscalls.def`, generated from `MPUBLICS.ASM` by `tools/gen_syscalls.sh`
      (number = (gate − 40h)/8). Used by both the kernel switch and the user stubs (`user/lib/libdoors.c`).
- [x] FPU/SSE for user code: CR0/CR4 set up, a 512-byte FXSAVE area per task, eager save/restore on every
      switch (moved from Phase 4).
- [x] Executable format: static ELF64 linked at 0x40000000 (`user/user.ld`) with separate R+X / R / RW
      segments; the loader (`kernel/job.c`) maps them with those permissions plus a 64 KB guarded stack.
      Until M7 programs are GRUB boot modules (found by name).
- [x] Jobs: `LoadNewJob`, `ExitJob`, `KillJob`, `GetJobNum`, `SetJobName`, `Get/SetCmdLine`, `INITIAL.JOB`;
      job teardown (tasks, requests, services, exchanges, address space); a CPU fault in ring 3 ends just
      that job (error 1000 + vector). `Chain`, `SetExitJob` and per-job video come with the CLI (Phase 8).
- [x] User-mode services: the request block pool is also mapped read-only at `RQB_UBASE`, so a ring-3
      service reads its requests as MMURTL services did; handles are translated at the syscall boundary.
- **Done when:** a ring-3 "hello" ELF runs as a job and exits cleanly. A bad pointer passed to a call
  returns an error instead of crashing the kernel. ✅ 2026-10-16: hello, badptr (16 bad calls rejected, its
  own page fault ends only that job), 2 FPU jobs bit-identical, user service ↔ user client in two jobs,
  KillJob of a 2-task job, 0 leaks over 6 jobs.

### Phase 7 — Drivers and file system (C sources port fairly directly)
- [x] Device interface (`kernel/device.c`, `include/doors/blk.h`): `DeviceOp`, `DeviceStat`, `DeviceInit`
      with MMURTL's device numbers (12/13 = hard disk 0/1) and ops (1 read, 2 write, 3 verify); 64-bit LBA.
      `InitDevDr` → drivers register themselves (no loadable drivers, as in MMURTL).
- [x] IDE: new ATA PIO driver (`drivers/ata.c`): LBA48/LBA28, size from IDENTIFY (no CHS, so none of the
      baseline's geometry trouble), polled, a kmutex per channel, ATAPI skipped. Replaces `HARDIDE.C`.
- [ ] Floppy, ISA DMA, RS232, parallel → later (not needed for the file system; floppy images can be
      superfloppy FAT volumes on any disk).
- [x] Block cache (`kernel/bcache.c`, 512 KB LRU, write-through) and partitions (`kernel/vol.c`): MBR, GPT
      behind a protective MBR, superfloppy FAT; drive letters from C:.
- [ ] Virtio-blk / AHCI and interrupt-driven I/O → with ext4 (M8) or later; PIO is fine for QEMU tests.
- **Done when:** sectors on a QEMU disk can be read and written through the block cache. ✅ 2026-10-16
  (MBR disk; GPT parsing is in place but not yet exercised by a test).

### Phase 7b — File system
The file system service keeps the original MMURTL API from `MFILES.H` with the same names and parameter order:
`CreateFile`, `CreateDir`, `DeleteDir`, `OpenFile`, `RenameFile`, `GetFileSize`, `SetFileSize`,
`ReadBlock`, `WriteBlock`, `ReadBytes`, `WriteBytes`, `GetFileLFA`, `SetFileLFA`, `CloseFile`, `DeleteFile`.
- Names are already passed as `(pName, cbName)`, so long names and UTF-8 need no API change.
  Raise the internal path limit (for example to 4096).
- File sizes and positions widen to 64-bit: `dSize`, `pdSizeRet`, `dLFA`, `pdLFARet`, `nBytes`
  become `int64_t`/`uint64_t`. This is the only signature change.
- `GetDirSector` is FAT-specific. Keep it for compatibility on FAT volumes; on ext4 it returns
  `ErcNotSupported`. **Done (M8):** `ReadDir(pPath, cbPath, dIndex, pEntryRet)` is system call 110, the
  first DOORS extension after MMURTL's 0–109 (`tools/gen_syscalls.sh` appends it, so numbering is unchanged).
  It returns one `struct dir_entry` (`include/doors/dirent.h`: name, 64-bit size, attributes, Unix mtime)
  and `ErcNoMatch` past the end, on every file system.
- `DeleteDir`, which the original never finished (`PROBLEMS.TXT` 2a), gets implemented. ✅ (M7)

Steps:
- [x] VFS layer. `kernel/fsys.c`: FILESYS service at priority 5 as in MMURTL, per-job handles, MMURTL
      sharing rules, files closed at job end. Each volume has a `struct fs_ops` table (`include/doors/fs.h`:
      lookup/create/remove/rename/read/write/truncate/readdir), and FAT and ext4 plug in side by side.
      Read-only volumes refuse writes with `ErcReadOnly`.
- [x] FAT driver (`kernel/fat.c`): FAT12/16/32 by cluster count, VFAT long names (read and create, with
      `NAME~N` aliases), case-insensitive paths with `.`/`..`, create/delete/rename/move (fixing `..`),
      mkdir/rmdir, byte-exact stream and block I/O, truncate/extend, RTC timestamps, all FAT copies
      updated, FAT32 FSInfo marked unknown. Checked by `fsck.fat` after every `make test`.
- [x] All `MFILES.H` calls (sizes/LFA 64-bit) from the kernel and from ring 3; `LoadNewJob` loads ELF files
      by path; `GetSystemDisk`.
- [x] ext4 read (M8, `kernel/ext4.c`): Linux's default `mkfs.ext4` features (64bit, extents, flex_bg,
      metadata_csum, dir_index, huge_file, ...). Superblock, 64-bit group descriptors, inodes and extent
      trees of any depth, holes and unwritten extents. htree directories are read linearly (their leaves are
      ordinary blocks). Checksums are verified on read (CRC32C, `kernel/crc32c.c`). Names are case-sensitive.
- [x] ext4 read/write (M8): block and inode allocation (bitmaps, group descriptors, flex_bg counters and
      every checksum kept up to date), extent insert with a depth 0→1 tree and leaf splits, truncate, sparse
      extend, directory insert/delete with the checksum tail, rename/move (fixes `..` and link counts),
      mkdir/rmdir. Limits: extent tree depth ≤ 1 on write, no writes into htree (`dir_index`-flagged)
      directories, no journal. A volume that needs journal recovery mounts read-only.
- [ ] Journaling (jbd2): replay on mount, then ordered-mode metadata journaling for writes. → M8b
- [ ] htree directory writes and deeper extent trees on write. → M8b
- [~] Image tool: `tools/mkdisk.py` adds an ext4 partition (F:) built with `mkfs.ext4 -d`. An ext4 *system*
      disk holding the `MMSYS` contents, booted from, comes in M8b/M9.
- [x] Test suite: M8 self-tests (read, write, a 5 GB sparse file, long and case-different names, rename,
      move, truncate, delete) plus a random stress test of 400 operations against a model of the expected
      file contents, with a new seed every run. Afterwards `tools/check_disk.py` runs `e2fsck -fn` and checks
      the contents with `debugfs`. Clean on repeated runs.
- Unsupported features (inline data, encryption, compression and similar) refuse a rw mount and allow ro
      where safe. Mount checks incompat flags.
- **Done when:** DOORS boots from an ext4 root, the CLI copies/renames/deletes files with long names,
  files larger than 4 GB work, and `e2fsck` reports the image clean after the stress test.

### Phase 8 — User land (M9)
The CLI and editor need about 25 calls that are not ported yet, plus a small C library. Work is split into
steps, each a release (0.9.x). 1.0.0 = the smoke test passes.
- [x] **9a: virtual video and keyboard (0.9.0).** Each job gets an 80×25 virtual screen (MMURTL `VIDEO.ASM`):
      `SetVidOwner`, `GetVidOwner`, `SetNormVid`, `GetNormVid`, `ClrScr`, `TTYOut` (per job, attributes),
      `PutVidChars`, `PutVidAttrs`, `GetVidChar`, `SetXY`, `GetXY`, `ScrollVid`, `Beep`, `Tone`.
      Keyboard service as in `KEYBOARD.ASM`: MMURTL key codes (key | shift state << 8 | locks << 16 |
      numpad << 24), the keyboard follows the video owner, `ReadKbd(pKeyCodeRet, fWait)`
      (`ErcNoKeyAvail` 700), global keys in a kernel task: Ctrl-Alt-PgDn = next job's screen,
      Ctrl-Alt-Del = kill the job on screen. Kernel output (`kprintf`) is job 1's screen; everything
      written to any screen is also copied to COM1 so tests can see it. The monitor gets MMURTL's status
      line (clock, ticks) and function keys (F1 load CLI, F2 jobs, F3 stats) next to its command line.
- [ ] **9b: job calls and C library (0.9.1).** `SetPath`/`GetPath`, `SetUserName`/`GetUserName`,
      `SetExitJob`/`GetExitJob`, `Chain`, `SetSysIn`/`SetSysOut`/`GetSysIn`/`GetSysOut`, `GetCMOSTime`/
      `GetCMOSDate`; `EditLine` in the user library (it only uses public calls). libc: `sprintf`, string and
      `ctype` functions, `fopen`/`fgets`/`fclose` over the file calls.
- [ ] **9c: CLI (0.9.2).** `port64/user/progs/cli.c` from `MSAMPLES/CLI/CLI.C` with as few changes as possible
      (the original stays untouched); `HELP.CLI` on the system disk.
- [ ] **9d: editor and samples (0.9.3).** `EDIT.C`, `BUSYLOOP`, `SERVICE`/`TESTSVC`.
- [ ] **9e: smoke test (1.0.0).** `docs/SMOKE_TEST.md` 64-bit column, scripted like `tools/boot32/smoke.py`.
      The debugger row (P7) moves to M10 with the debugger itself.
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
  custom DOORS format (no host tooling).

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
| MFILES.H | include/doors/files.h | same names; sizes/offsets 64-bit; `ReadDir` added |
| MONITOR.C | services/monitor.c | port |
| DEBUGGER / UASM.C | services/debugger.c | rewrite last |
| NUMCNVRT / MISCCODE | libc / kernel/util.c | rewrite in C |
| CM32 / DASM / MSCODE16 | — | replaced by GCC/NASM/GRUB; kept as reference |
