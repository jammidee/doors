; DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
; DOORS x86-64 interrupt entry stubs and GDT loading.
;
; Every vector 0..255 gets a stub that makes the stack look the same (pushing a dummy error code
; where the CPU doesn't), pushes the vector number, then jumps to isr_common, which saves all
; general registers into a struct regs (include/doors/cpu.h) and calls isr_dispatch(regs*).

bits 64
section .text

extern isr_dispatch

; Vectors where the CPU pushes an error code.
%define HAS_ERR(v) ((v) == 8 || ((v) >= 10 && (v) <= 14) || (v) == 17 || (v) == 21 || (v) == 29 || (v) == 30)

%macro ISR 1
isr_stub_%1:
%if HAS_ERR(%1)
    push qword %1
%else
    push qword 0
    push qword %1
%endif
    jmp isr_common
%endmacro

%assign v 0
%rep 256
    ISR v
%assign v v + 1
%endrep

isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp                        ; struct regs *
    call isr_dispatch                   ; stack is 16-byte aligned here (see cpu.h layout)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                         ; vector + error code
    iretq

; void gdt_load(const struct gdt_ptr *p, U16 code_sel, U16 data_sel, U16 tss_sel)
global gdt_load
gdt_load:
    lgdt [rdi]
    mov ds, dx
    mov es, dx
    mov ss, dx
    xor eax, eax
    mov fs, ax
    mov gs, ax
    ltr cx
    push rsi                            ; reload CS with a far return
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    ret

section .rodata
global isr_stub_table
align 8
isr_stub_table:
%assign v 0
%rep 256
    dq isr_stub_ %+ v
%assign v v + 1
%endrep
