; Software task switch (replaces MMURTL's JMP FWORD PTR [TSS] hardware task switch).
;
; void switch_to(U64 *save_rsp, U64 new_rsp)
;   Saves the callee-saved registers on the current stack, stores RSP in *save_rsp, loads
;   new_rsp and pops the next task's registers. Everything else (caller-saved registers, RFLAGS,
;   the interrupted user/kernel state) is already on that task's own stack: either in the
;   C frames that called the scheduler, or in the interrupt frame of isr.asm when it was
;   preempted by the timer.
;
; A new task's stack is prepared by sched.c so that switch_to "returns" into task_trampoline
; with r12 = entry and r13 = argument.

bits 64
section .text

global switch_to
global task_trampoline
extern task_started
extern task_exit

switch_to:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

task_trampoline:
    call task_started                   ; reap dead tasks, enable interrupts
    mov rdi, r13
    call r12                            ; entry(arg)
    call task_exit                      ; does not return
.hang:
    hlt
    jmp .hang
