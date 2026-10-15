; Fault-safe memory probes.
;
;   bool probe_read(const void *addr, U64 *val)    true = read ok, *val set
;   bool probe_write(void *addr, U64 val)          true = write ok
;
; If the marked instruction faults (#PF or #GP), idt.c's fault fixup table moves RIP to the
; matching fixup label, which returns false. Used by the self-tests now, and later to check
; pointers passed in from user mode (M6) without crashing the kernel.

bits 64
section .text

global probe_read
global probe_write
global probe_fixups

probe_read:
.insn:
    mov rax, [rdi]
    mov [rsi], rax
    mov eax, 1
    ret
.fix:
    xor eax, eax
    ret

probe_write:
.insn:
    mov [rdi], rsi
    mov eax, 1
    ret
.fix:
    xor eax, eax
    ret

section .rodata
align 8
; { faulting RIP, fixup RIP } pairs, terminated by 0.
probe_fixups:
    dq probe_read.insn,  probe_read.fix
    dq probe_write.insn, probe_write.fix
    dq 0, 0
