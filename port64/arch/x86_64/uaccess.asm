; Copying to and from user memory with fault recovery (see probe.asm / idt.c fault_fixup).
;
;   U64 user_copy(void *dst, const void *src, U64 n)   returns the number of bytes NOT copied
;
; If the rep movsb faults (unmapped page, or a write to a read-only user page with CR0.WP on),
; the fixup returns the remaining count instead of crashing the kernel. The C callers
; (kernel/syscall.c) check the address range first, so a user can only make it fault on user
; addresses, never point it into the kernel.

bits 64
section .text

global user_copy
global uaccess_fixups

user_copy:
    mov rcx, rdx
.insn:
    rep movsb
    xor eax, eax
    ret
.fix:
    mov rax, rcx                        ; bytes left
    ret

section .rodata
align 8
uaccess_fixups:
    dq user_copy.insn, user_copy.fix
    dq 0, 0
