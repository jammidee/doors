; DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.
; SYSCALL entry and the first entry into ring 3.
;
; MMURTL user programs called the OS through call gates (CALL FWORD PTR _Name, MPUBLICS.ASM).
; DOORS uses SYSCALL: RAX = call number (the gate's position in MPUBLICS order, syscalls.def),
; RDI = user pointer to the argument array (every argument as a U64, in MMURTL order).
; The result (an erc_t) comes back in RAX. RCX and R11 are clobbered (SYSCALL/SYSRET use them).
;
; SYSCALL doesn't switch stacks, so the entry does: interrupts are masked by IA32_FMASK until the
; user RSP is safe on the task's own kernel stack (syscall_kstack, set on every task switch).

bits 64
section .text

global syscall_entry
global enter_user
extern syscall_dispatch
extern syscall_kstack

%define USER_CS 0x2B                    ; SEL_UCODE | 3
%define USER_SS 0x23                    ; SEL_UDATA | 3

section .bss
user_rsp_tmp: resq 1

section .text
syscall_entry:
    mov [rel user_rsp_tmp], rsp
    mov rsp, [rel syscall_kstack]
    push qword [rel user_rsp_tmp]       ; user RSP
    push rcx                            ; user RIP
    push r11                            ; user RFLAGS
    push rbx                            ; keep the stack 16-byte aligned for the call
    sti                                 ; preemptible from here on, like any kernel code
    mov rsi, rdi                        ; args
    mov rdi, rax                        ; number
    call syscall_dispatch               ; rax = erc
    cli
    pop rbx
    pop r11
    pop rcx
    pop rsp                             ; back on the user stack (interrupts still off)
    xor edx, edx                        ; don't leak kernel values in scratch registers
    xor esi, esi
    xor edi, edi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    o64 sysret

; void enter_user(U64 rip, U64 rsp): start a new user task in ring 3 (interrupts on, clean
; registers, the task's FPU state already loaded by the switch).
enter_user:
    cli
    push USER_SS
    push rsi                            ; user RSP
    push 0x202                          ; RFLAGS: IF
    push USER_CS
    push rdi                            ; user RIP
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq
