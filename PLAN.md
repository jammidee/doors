# Plan: DOORS 64-bit

How the 64-bit version of DOORS (formerly MMURTL) gets built. This is the execution plan: strategy,
milestones, verification and workflow. The technical design (module mapping, data structures, file
system details) is in [docs/PORT64_PLAN.md](docs/PORT64_PLAN.md), and daily progress goes in [devlogs/](devlogs/).

## Goal

A 64-bit (x86-64 long mode) DOORS that:
- keeps the MMURTL design: exchanges, messages, `Request`/`Respond`, named services, jobs with their own
  address spaces;
- keeps **every public API name, parameter order and error code**, with only pointer and size types widening;
- uses a **modern file system (ext4)** behind a VFS, keeping FAT for exchanging files;
- boots in QEMU from GRUB without DOS, then on real hardware.

## Strategy

1. **Re-implement, don't convert.** Long mode has no hardware task switching and only restricted call gates,
   and CM32/DASM can't emit 64-bit code. The original source is the specification; the new code is written in
   C + NASM in a separate `port64/` tree.
2. **The original stays the reference.** The 32-bit system boots (`tools/boot32/run.sh`), so every behavior
   can be compared side by side: same command, same expected result.
3. **Bottom-up, always bootable.** Each milestone ends with an image that boots in QEMU and shows something
   new. Nothing is merged that breaks the boot.
4. **Test what can be tested off-target.** Pure logic (queues, exchanges, allocators, ext4 parsing) is written
   so it also compiles as a host program with unit tests. Hardware-facing code is checked in QEMU with
   scripted screen and serial checks.
5. **Behavior first, then cleanup.** Port semantics faithfully, including odd ones, and document them.
   Fix original bugs only after confirming them (for example the `__Request` `dData2` offset).

## Milestones

Each milestone has a demo (what you'll see) and acceptance checks (what must pass before moving on).

| # | Milestone | Demo | Acceptance |
|---|---|---|---|
| M0 | **Baseline** ✅ | Original MMURTL V1.0 boots in QEMU; CLI, editor, jobs, IPC, debugger work | `docs/SMOKE_TEST.md` run on 32-bit ✅; `tools/boot32/smoke.py` PASS 20 / XFAIL 3 ✅ |
| M1 | **Hello, long mode** ✅ | `DOORS` 64-bit banner from GRUB-booted kernel | Boots via `make run`; serial log matches; triple-fault-free on QEMU `-d int` |
| M2 | **Interrupts & time** ✅ | Ticking clock and keyboard echo | Exceptions print register dumps; PIT 10 ms tick matches original rate |
| M3 | **Memory** ✅ | Free/used page stats on screen | Allocator self-tests pass; aliasing works across two address spaces |
| M4 | **Tasks** ✅ | Two kernel tasks at different priorities interleaving | Preemption and priority order match the 32-bit scheduler on the same scenario |
| M5 | **Messaging** ✅ | Test service answering `Request`s | Host unit tests + in-kernel IPC tests for every primitive (`SendMsg`…`MoveRequest`) |
| M6 | **User mode & jobs** ✅ | Ring-3 ELF "hello" job started from `INITIAL.JOB` | Bad pointers return errors (no kernel crash); jobs exit and free all memory |
| M7 | **Disks & FAT** ✅ | `dir` of a FAT image from a test job | Read/write FAT12/16/32 matches host `mtools` view of the same image |
| M8 | **ext4** ✅ | ext4 volume read/write; long names; >4 GB file | Random stress test (new seed each `make test`), then host `e2fsck -fn` reports clean ✅ |
| M8b | **ext4 journal & root** (journal ✅ 1.1.0, htree ✅ 1.2.0) | jbd2 replay + journaled writes ✅; htree directory writes ✅; boot from an ext4 system disk | Power-cut test (kill QEMU mid-write), then replay and `e2fsck -fn` clean ✅ (`make powercut`) |
| M9 | **User land** ✅ (1.0.0) | Monitor + CLI + editor running on 64-bit | `docs/SMOKE_TEST.md` passes on 64-bit, same as on 32-bit ✅ (24 pass, P7 debugger moved to M10) |
| M10 | **Optional** | — | Debugger/disassembler, 64-bit CM32/DASM, UEFI, APIC, SMP |

M7 and M8 can overlap with M9: the CLI can start on FAT before ext4 is finished.

After 1.0, [ROADMAP.md](ROADMAP.md) lays out the way from here to a stable OS (storage, testing, kernel hardening,
real hardware, SMP, security, a usable system, release engineering).

## How each step is verified

- **Scripted QEMU runs.** `make test` boots the image headless and reads the screen (as `qscreen.py` does
  for 32-bit) and the serial log, checking for expected lines with a timeout. Every milestone adds its checks.
- **Serial debug console** from M1 onward. All kernel logs go to COM1 as well as the screen, so failures can
  be read without the screen.
- **GDB** through the QEMU gdbstub (`-s -S`) for anything that faults.
- **Host unit tests** for the logic modules (ready queue, exchanges, link-block pools, page allocator,
  ext4 extent tree), built with the host `gcc` and run in seconds.
- **Side-by-side checks** against the 32-bit system for user-visible behavior (CLI output, error codes).
- **`e2fsck`** as the judge for every ext4 write test.

## Workflow

- Work happens on short branches per milestone step (for example `m1-longmode-boot`) and is merged to `main`
  when it boots and passes the checks.
- Each working session is appended to `devlogs/YYYY-MM-DD.md`: what was done, findings, decisions, next steps.
- The checklists in `docs/PORT64_PLAN.md` get ticked as items land, and this file's milestone table gets
  updated (✅).
- The original files are never edited. Fixes for the 32-bit baseline go in `tools/boot32/`.
- Commit messages say which milestone they belong to (for example `M1: long-mode trampoline`).

## First concrete steps

1. Commit the current work (`tools/boot32/`, `devlogs/`, doc updates).
2. **Finish M0:** run CLI commands (`type`, `copy`, `md`, `rd`, `ren`, `del`, `run`), the editor, the external
   commands from `COMMANDS.CLI`, job switching and the debugger. Record the results in `docs/SMOKE_TEST.md`
   and turn them into a scripted check.
3. **Start M1:**
   - Create the `port64/` skeleton: `Makefile`, `linker.ld`, `boot/`, `arch/x86_64/`, `kernel/`.
   - Multiboot2 header, 32-bit entry, page tables, switch to long mode, jump to `kmain`.
   - VGA text output and COM1 serial output, then `make run` and `make test`.
4. Settle the shared headers: `include/doors/` with the public API and error codes taken from
   `MKERNEL.H`, `MJOB.H`, `MFILES.H`, `MOSEDF.INC`, plus `_Static_assert` size checks.

## Risks to watch

| Risk | Mitigation |
|---|---|
| Kernel semantics hidden in hand-written ASM (register contracts) | Read each primitive closely, write a test for it, and port the behavior rather than the instructions |
| ext4 write path is large (extents, allocation, journal) | Read-only first; writes behind a mount flag until the `e2fsck` stress test passes |
| Original bugs come back (double fault under FS load, Ctrl-Alt-Del switching) | Stress tests in M5/M8; watch for them specifically |
| Scope creep (UEFI, SMP, new drivers) | Everything optional stays in M10 until M9 passes |
| Struct layouts from `.INC` offsets | Rebuild as C structs, check sizes with `_Static_assert`, never copy numeric offsets |

## Open questions

- Confirm ext4 as the native format (alternatives: ext2 for simplicity, or a custom DOORS format).
- Keep the `.RUN` executable format as a second loader for old programs, or support ELF64 only?
