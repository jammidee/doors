; DOORS x86-64 boot code.
;
; GRUB loads the kernel ELF via Multiboot2 and enters _start in 32-bit protected mode
; (paging off, EAX = Multiboot2 magic, EBX = physical address of the boot information).
; This file:
;   1. checks the magic and that the CPU supports long mode,
;   2. builds boot page tables mapping the first 1 GiB twice:
;        identity  0x0000000000000000 -> 0   (so this code keeps running after paging is on)
;        higher    0xFFFFFFFF80000000 -> 0   (where the C kernel is linked)
;      using 2 MiB pages,
;   3. enables PAE + long mode + paging, loads a 64-bit GDT and far-jumps to 64-bit code,
;   4. switches to the kernel stack and calls kmain(magic, mbi_phys).
;
; Everything here lives in the .boot* sections, linked at its physical address (1 MiB+).

KERNEL_VMA      equ 0xFFFFFFFF80000000
MB2_MAGIC       equ 0xE85250D6          ; header magic
MB2_LOADER      equ 0x36D76289          ; magic GRUB passes in EAX
MB2_ARCH_I386   equ 0

PTE_P           equ 1 << 0              ; present
PTE_W           equ 1 << 1              ; writable
PTE_PS          equ 1 << 7              ; 2 MiB page (in a page directory)

;-----------------------------------------------------------------------------
section .multiboot
align 8
mb2_header:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd mb2_header_end - mb2_header
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))
    ; end tag
    dw 0                                ; type
    dw 0                                ; flags
    dd 8                                ; size
mb2_header_end:

;-----------------------------------------------------------------------------
section .boot.text
bits 32
global _start
extern kmain

_start:
    cli
    cld
    mov esp, boot_stack_top
    mov edi, eax                        ; keep magic and MBI for kmain (SysV: rdi, rsi)
    mov esi, ebx

    cmp eax, MB2_LOADER
    mov al, 'M'                         ; error code: not booted by Multiboot2
    jne boot_error

    ; CPUID must exist (ID flag in EFLAGS toggles) ...
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    mov al, 'C'                         ; no CPUID
    je boot_error

    ; ... and report long mode (CPUID 80000001h EDX bit 29).
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    mov al, 'L'                         ; no long mode
    jb boot_error
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    mov al, 'L'
    jz boot_error

    ; Page tables (in .boot.bss, zeroed by the loader).
    mov eax, boot_pdpt_lo
    or eax, PTE_P | PTE_W
    mov [boot_pml4], eax                ; PML4[0]   -> identity PDPT
    mov eax, boot_pdpt_hi
    or eax, PTE_P | PTE_W
    mov [boot_pml4 + 511 * 8], eax      ; PML4[511] -> higher-half PDPT

    mov eax, boot_pd
    or eax, PTE_P | PTE_W
    mov [boot_pdpt_lo], eax             ; PDPT_lo[0]   -> PD   (0 .. 1 GiB)
    mov [boot_pdpt_hi + 510 * 8], eax   ; PDPT_hi[510] -> PD   (0xFFFFFFFF80000000 ..)

    xor ecx, ecx                        ; PD[i] = i * 2 MiB, 512 entries = 1 GiB
.fill_pd:
    mov eax, ecx
    shl eax, 21
    or eax, PTE_P | PTE_W | PTE_PS
    mov [boot_pd + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .fill_pd

    mov eax, boot_pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5                      ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, 1 << 8                      ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)        ; CR0.PG | CR0.PE
    mov cr0, eax                        ; now in compatibility mode

    lgdt [boot_gdt_ptr]
    jmp 0x08:long_mode_entry            ; 64-bit code segment

; Print "DOORS boot error: <AL>" on the VGA text screen and stop.
boot_error:
    mov ebx, 0xB8000
    mov ecx, boot_err_msg
.msg:
    mov dl, [ecx]
    test dl, dl
    jz .code
    mov dh, 0x4F                        ; white on red
    mov [ebx], dx
    add ebx, 2
    inc ecx
    jmp .msg
.code:
    mov ah, 0x4F
    mov [ebx], ax
.halt:
    hlt
    jmp .halt

bits 64
long_mode_entry:
    mov ax, 0x10                        ; 64-bit data segment
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov rsp, kernel_stack_top           ; higher-half stack (sign-extended imm32)
    xor rbp, rbp
    mov edi, edi                        ; zero-extend magic and MBI address
    mov esi, esi
    mov rax, kmain                      ; absolute higher-half address
    call rax
.hang:
    cli
    hlt
    jmp .hang

;-----------------------------------------------------------------------------
section .boot.data
align 8
boot_gdt:
    dq 0                                ; 0x00 null
    dq 0x00AF9A000000FFFF               ; 0x08 64-bit code, DPL 0 (L=1)
    dq 0x00CF92000000FFFF               ; 0x10 data, DPL 0
boot_gdt_end:

boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dq boot_gdt

boot_err_msg:
    db "DOORS boot error: ", 0

;-----------------------------------------------------------------------------
section .boot.bss nobits alloc write align=4096
boot_pml4:      resb 4096
boot_pdpt_lo:   resb 4096
boot_pdpt_hi:   resb 4096
boot_pd:        resb 4096
boot_stack:     resb 4096
boot_stack_top:

;-----------------------------------------------------------------------------
; Kernel stack, in the higher half (normal .bss).
section .bss nobits alloc write align=16
global kernel_stack_top
kernel_stack:   resb 16384
kernel_stack_top:
