# DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
# DOORS x86-64 build. Run in WSL/Linux (gcc, nasm, ld, grub-mkrescue, xorriso, qemu).
#
#   make            build build/kernel.elf and build/doors.iso
#   make run        boot in QEMU (window; serial log on this terminal)
#   make console    boot in QEMU drawn in this terminal (curses); used by doors.bat
#   make run-ext4   boot from build/sysdisk.img: GRUB on the disk, C: = ext4 system volume, no ISO
#   make smoke      docs/SMOKE_TEST.md on 64-bit: CLI, editor, samples driven by typed keys (tools/smoke64.py),
#                   on the FAT32 system disk (booted from the ISO) and on the ext4 one (booted from the disk)
#   make powercut   kill QEMU during journaled ext4 writes, check replay + e2fsck (tools/powercut.py)
#   make fuzz       damaged FAT/ext4 disks against the kernel's file system code, built for Linux
#                   with sanitizers (tests/fsfuzz); FUZZ_N damaged images per disk and build
#   make test       boot headless (tools/qemu_test.py); pass if the serial log shows every
#                   TEST_MARKS string and the kernel exits QEMU through isa-debug-exit
#   make fulltest   make test plus every volume filled to 100 % and freed (ROADMAP 1.7); ~5 min
#   make analyze    static analysis of the kernel: gcc -fanalyzer, cppcheck, clang --analyze (tools/analyze.sh)
#   make debug      like run, but QEMU waits for gdb on :1234 (gdb build/kernel.elf -ex 'target remote :1234')
#   make clean
#
# QEMU_EXTRA="..." adds QEMU options to run/debug, e.g. a monitor socket for tools/boot32/qscreen.py:
#   make run QEMU_EXTRA="-monitor unix:/tmp/doors.mon,server,nowait"

BUILD    := build
CC       := gcc
AS       := nasm
LD       := ld
QEMU     := qemu-system-x86_64

CFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Werror \
            -ffreestanding -fstack-protector-strong -mstack-protector-guard=global \
            -fno-pic -fno-pie -fno-builtin \
            -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
            -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
            -Iinclude -I$(BUILD)/gen -MMD -MP
ASFLAGS  := -f elf64 -g -F dwarf
LDFLAGS  := -n -nostdlib -T linker.ld -z max-page-size=0x1000

C_SRCS   := $(shell find kernel drivers arch -name '*.c' 2>/dev/null)
ASM_SRCS := $(shell find boot arch -name '*.asm' 2>/dev/null)
OBJS     := $(ASM_SRCS:%.asm=$(BUILD)/%.o) $(C_SRCS:%.c=$(BUILD)/%.o)

KERNEL   := $(BUILD)/kernel.elf
ISO      := $(BUILD)/doors.iso
TEST_ISO := $(BUILD)/doors-test.iso

QEMU_BASE := -m 256M -no-reboot -no-shutdown
QEMU_EXTRA ?=
# Milestone checks for `make test`: kernel/selftest.c prints these lines, tools/qemu_test.py
# checks them (--expect) and types into the guest when a trigger line appears (--send).
TEST_TIMEOUT := 240
TEST_ARGS := \
	--expect "DOORS: M1 OK" \
	--expect "M2: timer ok" \
	--expect "M2: breakpoint ok" \
	--expect "M2: keyboard ok" \
	--expect "M2: exception dump ok" \
	--expect "DOORS: M2 OK" \
	--expect "M3: pmm ok" \
	--expect "M3: paging ok" \
	--expect "M3: AllocOSPage/DeAllocPage/QueryPages ok" \
	--expect "M3: AllocDMAPage ok" \
	--expect "M3: address spaces ok" \
	--expect "M3: stack overflow caught by the guard page" \
	--expect "DOORS: M3 OK" \
	--expect "M4: priority ok" \
	--expect "M4: sleep ok" \
	--expect "M4: round-robin ok" \
	--expect "M4: context switch ok" \
	--expect "M4: NewTask/SpawnTask/SetPriority/GetPriority ok" \
	--expect "DOORS: M4 OK" \
	--expect "M5: exchanges ok" \
	--expect "M5: WaitMsg/SendMsg ok" \
	--expect "M5: ping-pong ok" \
	--expect "M5: Alarm/KillAlarm ok" \
	--expect "M5: keyboard ISendMsg ok" \
	--expect "M5: Request/Respond ok" \
	--expect "M5: cross-job Request ok" \
	--expect "M5: no leaks" \
	--expect "DOORS: M5 OK" \
	--expect "hello: running in ring 3 as job" \
	--expect "hello: KECHO replied 'kernel got: ring 3'" \
	--expect "M6: hello ok" \
	--expect "badptr: all" \
	--expect "M6: bad pointers ok" \
	--expect "M6: FPU/SSE ok" \
	--expect "upcli: UPCASE replied 'DOORS FROM USER MODE'" \
	--expect "M6: user-mode service ok" \
	--expect "M6: KillJob ok" \
	--expect "M6: no leaks" \
	--expect "DOORS: M6 OK" \
	--expect "M7: disk ok" \
	--expect "M7: FAT read ok" \
	--expect "M7: FAT write ok" \
	--expect "M7: LoadNewJob from a file ok" \
	--expect "fsuser: all file calls from ring 3 ok" \
	--expect "M7: file calls from ring 3 ok" \
	--expect "DOORS: M7 OK" \
	--expect "FAT on E: was not unmounted cleanly (dirty flag set); checking it" \
	--expect "FAT on E: check: 4 problems" \
	--expect "FAT on H: check: 5 problems" \
	--expect "FAT on H: mounted read-only" \
	--expect "ext4 on F: a block logged in transaction 2 has a bad checksum: the log ends there" \
	--expect "ext4 on F: journal replayed: 1 transactions, 1 blocks" \
	--expect "M8: ext4 read ok" \
	--expect "M8: ext4 write ok" \
	--expect "M8: ext4 stress ok" \
	--expect "M8b: htree directory writes ok" \
	--expect "M8b: deep extent trees ok" \
	--expect "M8: LoadNewJob from ext4 ok" \
	--expect "DOORS: M8 OK" \
	--expect "vidtest: all video and keyboard calls ok" \
	--expect "M9: video and keyboard ok" \
	--expect "jobtest: job calls, Chain, exit job and C library ok" \
	--expect "M9: job calls ok" \
	--expect "hello: command line 'hello'" \
	--expect "M9: CLI ok" \
	--expect "NUMBERS Service gave out number: 1." \
	--expect "M9: samples ok" \
	--expect "1.7: edge cases ok" \
	--expect "2.4: kernel invariants ok" \
	--send "M2: type doors=doors\n" \
	--send "M5: type msg=msg\n" \
	--send "M9: type kbd=kQ{f1}{up}\n" \
	--send "M9: type edit=hellx{backspace}o\n" \
	--send "M9: type cli1=md C:\T9\ncopy C:\HELLO.TXT C:\T9\H.TXT\n" \
	--send "M9: type cli2=hello\n" \
	--send "M9: type ed1=edit C:\T9\EDITED_BY_DOORS.TXT\n" \
	--send "M9: type ed2=y" \
	--send "M9: type ed3=Edited in DOORS{alt-s}{alt-q}" \
	--send "M9: type cli3=dir C:\T9\nexit\n"

.PHONY: all run run-ext4 console test fulltest smoke powercut fuzz analyze crashtest debug clean FORCE
# Keep object files that make would otherwise treat as intermediate and delete (user/lib, user/progs).
.SECONDARY:

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Version (VERSION file) and git commit, as build/gen/doors/version.h. It is rewritten only when its
# content changes, so only kernel/main.c recompiles after a version bump or a commit.
DOORS_VERSION := $(shell grep -v '^\#' VERSION | head -n 1)
DOORS_COMMIT  := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
VERSION_H     := $(BUILD)/gen/doors/version.h

$(VERSION_H): FORCE
	@mkdir -p $(dir $@)
	@{ echo '/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */'; \
	   echo '/* Generated by the Makefile from port64/VERSION and git. Do not edit. */'; \
	   echo '#define DOORS_VERSION "$(DOORS_VERSION)"'; \
	   echo '#define DOORS_COMMIT  "$(DOORS_COMMIT)"'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

$(BUILD)/kernel/main.o: $(VERSION_H)

FORCE:

# Two links: the first (empty function table) gives the addresses, tools/gen_ksyms.sh turns them
# into build/gen/ksyms.c, the second links that table in (into .rodata: no function moves; the
# last line checks it) for the stack traces of kernel/debug.c.
KSYMS0 := $(BUILD)/gen/ksyms0.o
KSYMS  := $(BUILD)/gen/ksyms.o
$(BUILD)/gen/ksyms0.c: tools/gen_ksyms.sh
	@mkdir -p $(@D)
	sh tools/gen_ksyms.sh < /dev/null > $@
$(BUILD)/kernel0.elf: $(OBJS) $(KSYMS0) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) $(KSYMS0) -o $@
$(BUILD)/gen/ksyms.c: $(BUILD)/kernel0.elf tools/gen_ksyms.sh
	nm -n --defined-only $< | sh tools/gen_ksyms.sh > $@
$(BUILD)/gen/%.o: $(BUILD)/gen/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(KERNEL): $(OBJS) $(KSYMS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) $(KSYMS) -o $@
	@nm -n --defined-only $(BUILD)/kernel0.elf | awk '$$2 ~ /^[tT]$$/' > $(BUILD)/gen/text0.txt
	@nm -n --defined-only $@ | awk '$$2 ~ /^[tT]$$/' | cmp -s - $(BUILD)/gen/text0.txt || \
		{ echo "ksyms: the function table moved code; stack traces would be wrong"; rm -f $@; exit 1; }

# ---- user programs (port64/user): static ELF64 at 0x40000000, loaded as GRUB boot modules ----
USER_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
               -fno-pic -fno-pie -fno-builtin -mcmodel=small -fno-asynchronous-unwind-tables \
               -Iinclude -Iuser/lib -Iuser/lib/libc -MMD -MP
USER_PROGS  := $(basename $(notdir $(wildcard user/progs/*.c)))
USER_ELFS   := $(USER_PROGS:%=$(BUILD)/user/%.elf)
USER_LIB    := $(BUILD)/user/lib/crt0.o $(BUILD)/user/lib/libdoors.o $(BUILD)/user/lib/libc.o

$(BUILD)/user/lib/crt0.o: user/lib/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# Programs ported from MSAMPLES keep their 1990s char / unsigned char mixing (as few changes as possible).
MMURTL_PORTS := cli edit busyloop service testsvc
$(MMURTL_PORTS:%=$(BUILD)/user/progs/%.o): USER_CFLAGS += -Wno-pointer-sign -Wno-unused-variable \
	-Wno-unused-but-set-variable -Wno-misleading-indentation

$(BUILD)/user/%.elf: $(BUILD)/user/progs/%.o $(USER_LIB) user/user.ld
	$(LD) -nostdlib -T user/user.ld -z max-page-size=0x1000 $(USER_LIB) $< -o $@

# $(call mkiso,<iso>,<kernel cmdline>): GRUB menu with the kernel, every user program as a
# module named after it, and INITIAL.JOB.
define mkiso
	@rm -rf $@.d && mkdir -p $@.d/boot/grub $@.d/boot/user
	cp $(KERNEL) $@.d/boot/kernel.elf
	cp $(USER_ELFS) $@.d/boot/user/
	cp user/INITIAL.JOB $@.d/boot/INITIAL.JOB
	{ printf 'set timeout=0\nset default=0\nmenuentry "DOORS $(DOORS_VERSION)" {\n'; \
	  printf '\tmultiboot2 /boot/kernel.elf %s\n' '$(2)'; \
	  for p in $(USER_PROGS); do printf '\tmodule2 /boot/user/%s.elf %s\n' $$p $$p; done; \
	  printf '\tmodule2 /boot/INITIAL.JOB INITIAL.JOB\n\tboot\n}\n'; } > $@.d/boot/grub/grub.cfg
	grub-mkrescue -o $@ $@.d 2>/dev/null
endef

$(ISO): $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,)

$(TEST_ISO): $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,test)
FULLTEST_ISO := $(BUILD)/doors-fulltest.iso
$(FULLTEST_ISO): $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,test full)

# Power-cut test (tools/powercut.py): a writer, a writer that stops inside a journal commit, a checker;
# and a writer and checker on FAT (C:)
PC_ISOS := $(BUILD)/doors-pc.iso $(BUILD)/doors-pcstop.iso $(BUILD)/doors-pccheck.iso \
	$(BUILD)/doors-pcfat.iso $(BUILD)/doors-pcfatcheck.iso
$(BUILD)/doors-pcfat.iso: $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,powercut vol=C)
$(BUILD)/doors-pcfatcheck.iso: $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,pccheck vol=C)
$(BUILD)/doors-pc.iso: $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,powercut)
$(BUILD)/doors-pcstop.iso: $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,powercut stop=60)
$(BUILD)/doors-pccheck.iso: $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,pccheck)

# Disks: `make run` keeps build/disk.img between runs (delete it for a fresh one); `make test`
# builds a fresh build/disk-test.img, and tools/check_disk.py checks it with fsck.fat/mtools after.
DISK      := $(BUILD)/disk.img
TEST_DISK := $(BUILD)/disk-test.img
# the second test disk (primary slave, G:): GPT, ext4 with 1K blocks and htree directories
TEST_DISK2 := $(BUILD)/disk2-test.img
QEMU_DISK  = -drive file=$(1),format=raw,if=ide,index=0 -boot d

MKDISK_ARGS = --hello $(BUILD)/user/hello.elf --progs $(BUILD)/user --sys user/sys
$(DISK): $(USER_ELFS) $(wildcard user/sys/*) tools/mkdisk.py
	python3 tools/mkdisk.py $@ $(MKDISK_ARGS)

# An ext4 system disk that boots by itself (no ISO): GRUB in the MBR, C: = ext4 with /boot and \DOORS\
# (ROADMAP 1.6). Made from the normal ISO's /boot tree; `make run-ext4` boots it (kept between runs).
SYSDISK := $(BUILD)/sysdisk.img
$(SYSDISK): $(ISO) tools/mkdisk.py
	python3 tools/mkdisk.py $@ --system ext4 --boot $(ISO).d/boot $(MKDISK_ARGS)

run-ext4: $(SYSDISK)
	$(QEMU) $(QEMU_BASE) -name DOORS -drive file=$(SYSDISK),format=raw,if=ide,index=0 -boot c -serial stdio $(QEMU_EXTRA)

run: $(ISO) $(DISK)
	$(QEMU) $(QEMU_BASE) -name DOORS -cdrom $(ISO) $(call QEMU_DISK,$(DISK)) -serial stdio $(QEMU_EXTRA)

# The DOORS screen drawn in this terminal (doors.bat uses it); serial goes to build/serial-run.log.
# Quit: Alt+2 (or Esc then 2) for the QEMU monitor, then type quit.
console: $(ISO) $(DISK)
	$(QEMU) $(QEMU_BASE) -name DOORS -cdrom $(ISO) $(call QEMU_DISK,$(DISK)) -display curses \
		-serial file:$(BUILD)/serial-run.log $(QEMU_EXTRA)

debug: $(ISO) $(DISK)
	$(QEMU) $(QEMU_BASE) -name DOORS -cdrom $(ISO) $(call QEMU_DISK,$(DISK)) -serial stdio -s -S $(QEMU_EXTRA)

# QEMU_TEST: the QEMU `make test` runs, e.g. a script that adds -s to debug a hung test with gdb
QEMU_TEST ?= qemu-system-x86_64
test: $(TEST_ISO) $(USER_ELFS)
	python3 tools/mkdisk.py $(TEST_DISK) $(MKDISK_ARGS) --damage --disk2 $(TEST_DISK2)
	python3 tools/qemu_test.py --qemu $(QEMU_TEST) --iso $(TEST_ISO) --disk $(TEST_DISK) --disk2 $(TEST_DISK2) \
		--log $(BUILD)/serial.log --timeout $(TEST_TIMEOUT) $(TEST_ARGS)
	python3 tools/check_disk.py $(TEST_DISK) $(TEST_DISK2) --log $(BUILD)/serial.log

# make test plus every volume filled to 100 % and freed again (ROADMAP 1.7), then the same disk
# checks; ~5 minutes, most of it ATA PIO
fulltest: $(FULLTEST_ISO) $(USER_ELFS)
	python3 tools/mkdisk.py $(TEST_DISK) $(MKDISK_ARGS) --damage --disk2 $(TEST_DISK2)
	python3 tools/qemu_test.py --qemu $(QEMU_TEST) --iso $(FULLTEST_ISO) --disk $(TEST_DISK) --disk2 $(TEST_DISK2) \
		--log $(BUILD)/serial.log --timeout 600 $(TEST_ARGS) --expect "1.7: full volumes ok"
	python3 tools/check_disk.py $(TEST_DISK) $(TEST_DISK2) --log $(BUILD)/serial.log

# Debugging aids and crash reports, each triggered on purpose (ROADMAP 2.4, 2.7): a kernel with
# "crash=NAME" per case (kernel/debug.c crash_test), then a normal boot that must show the report
CRASH_TESTS := stack uaf double assert check fault panic
CRASH_ISOS  := $(CRASH_TESTS:%=$(BUILD)/doors-crash-%.iso)
$(BUILD)/doors-crash-%.iso: $(KERNEL) $(USER_ELFS) user/INITIAL.JOB
	$(call mkiso,$@,crash=$*)
crashtest: $(CRASH_ISOS) $(ISO) $(USER_ELFS)
	python3 tools/crashtest.py

# Static analysis (ROADMAP 2.5): gcc -fanalyzer, cppcheck, clang --analyze; tools/analyze.allow
# lists the intended findings
analyze: $(VERSION_H)
	sh tools/analyze.sh

# Kill QEMU in the middle of journaled ext4 writes, then check the disk with Linux and DOORS
powercut: $(PC_ISOS) $(USER_ELFS)
	python3 tools/powercut.py

# The file system fuzzer (tests/fsfuzz/fsfuzz.c): the kernel's volume, FAT, ext4 and cache code
# compiled for Linux. fsfuzz has ASan + UBSan; fsfuzz-stk runs the calls on a 12 KB guarded stack.
FSFUZZ_SRC := tests/fsfuzz/fsfuzz.c kernel/vol.c kernel/fat.c kernel/ext4.c kernel/crc32c.c kernel/bcache.c
FSFUZZ_CFLAGS := -std=gnu11 -g -Wall -Wno-unused-function -Itests/fsfuzz/include -Iinclude -I$(BUILD)/gen \
	-fno-omit-frame-pointer
FUZZ_N ?= 1500
FUZZ_DISKS := $(BUILD)/fuzz/fat.img $(BUILD)/fuzz/ext4.img $(BUILD)/fuzz/ext4c.img

$(BUILD)/host/fsfuzz: $(FSFUZZ_SRC) $(wildcard include/doors/*.h tests/fsfuzz/include/doors/*.h)
	@mkdir -p $(@D)
	$(CC) $(FSFUZZ_CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize=alignment \
		-fno-sanitize-recover=all $(FSFUZZ_SRC) -o $@
$(BUILD)/host/fsfuzz-stk: $(FSFUZZ_SRC) $(wildcard include/doors/*.h tests/fsfuzz/include/doors/*.h)
	@mkdir -p $(@D)
	$(CC) $(FSFUZZ_CFLAGS) -O2 -DFSFUZZ_STACK=12288 $(FSFUZZ_SRC) -o $@
$(FUZZ_DISKS) &: tools/fuzzdisk.py
	python3 tools/fuzzdisk.py $(BUILD)/fuzz

# Six runs in parallel (3 disks x 2 builds) with a new seed (the time) every `make fuzz`, so both
# builds see the same damaged images; the logs are build/fuzz/*.log, and each failure line there
# gives the command that reproduces it
fuzz: $(BUILD)/host/fsfuzz $(BUILD)/host/fsfuzz-stk $(FUZZ_DISKS)
	@for d in $(FUZZ_DISKS); do for f in fsfuzz fsfuzz-stk; do \
		$(BUILD)/host/$$f $$d -n $(FUZZ_N) > $${d%.img}-$$f.log 2>&1 & done; done; wait; \
	fail=0; for l in $(BUILD)/fuzz/*-fsfuzz*.log; do \
		grep -E "^(FAIL|fsfuzz .*damaged)" $$l; grep -q "^FSFUZZ PASS" $$l || fail=1; done; \
	if [ $$fail = 0 ]; then echo "FUZZ PASS"; else echo "FUZZ FAIL"; exit 1; fi

# docs/SMOKE_TEST.md on the normal ISO: types into a CLI and reads the screen (tools/smoke64.py)
smoke: $(ISO) $(USER_ELFS)
	python3 tools/smoke64.py
	python3 tools/smoke64.py --ext4

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(wildcard $(BUILD)/user/*/*.d)
