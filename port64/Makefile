# DOORS x86-64 build. Run in WSL/Linux (gcc, nasm, ld, grub-mkrescue, xorriso, qemu).
#
#   make            build build/kernel.elf and build/doors.iso
#   make run        boot in QEMU (window; serial log on this terminal)
#   make test       boot headless; pass if the serial log shows the milestone marker
#                   and the kernel exits QEMU through isa-debug-exit
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
            -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
            -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
            -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
            -Iinclude -MMD -MP
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
TEST_MARK := DOORS: M1 OK
TEST_TIMEOUT := 30

.PHONY: all run test debug clean

all: $(ISO)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# $(call mkiso,<iso>,<kernel cmdline>)
define mkiso
	@rm -rf $@.d && mkdir -p $@.d/boot/grub
	cp $(KERNEL) $@.d/boot/kernel.elf
	sed 's|@CMDLINE@|$(2)|' grub/grub.cfg > $@.d/boot/grub/grub.cfg
	grub-mkrescue -o $@ $@.d 2>/dev/null
endef

$(ISO): $(KERNEL) grub/grub.cfg
	$(call mkiso,$@,)

$(TEST_ISO): $(KERNEL) grub/grub.cfg
	$(call mkiso,$@,test)

run: $(ISO)
	$(QEMU) $(QEMU_BASE) -name DOORS -cdrom $(ISO) -serial stdio $(QEMU_EXTRA)

debug: $(ISO)
	$(QEMU) $(QEMU_BASE) -name DOORS -cdrom $(ISO) -serial stdio -s -S $(QEMU_EXTRA)

test: $(TEST_ISO)
	@rm -f $(BUILD)/serial.log
	@timeout $(TEST_TIMEOUT) $(QEMU) $(QEMU_BASE) -cdrom $(TEST_ISO) -display none \
		-serial file:$(BUILD)/serial.log \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04; \
	rc=$$?; \
	cat $(BUILD)/serial.log 2>/dev/null; \
	if [ $$rc -eq 33 ] && grep -q "$(TEST_MARK)" $(BUILD)/serial.log; then \
		echo "TEST PASS: $(TEST_MARK)"; \
	else \
		echo "TEST FAIL (qemu exit $$rc; 124 = timeout)"; exit 1; \
	fi

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
