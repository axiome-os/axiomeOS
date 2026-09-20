.intel_syntax noprefix
.section .text

.macro ISR_NOERRCODE n
.align 8
.global isr\n
isr\n:
    push 0
    push \n
    jmp isr_common
.endm

.macro ISR_ERRCODE n
.align 8
.global isr\n
isr\n:
    push \n
    jmp isr_common
.endm

isr_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    mov rdi, rsp
    call isr_handler

    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16
    iretq

ISR_NOERRCODE  0
ISR_NOERRCODE  1
ISR_NOERRCODE  2
ISR_NOERRCODE  3
ISR_NOERRCODE  4
ISR_NOERRCODE  5
ISR_NOERRCODE  6
ISR_NOERRCODE  7
ISR_ERRCODE    8
ISR_NOERRCODE  9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

.global isr_timer
isr_timer:
    push 0
    push 0x20
    jmp isr_common

.global isr_kbd
isr_kbd:
    push 0
    push 0x21
    jmp isr_common

.global isr_mouse
isr_mouse:
    push 0
    push 0x22
    jmp isr_common

.global isr_serial
isr_serial:
    push 0
    push 0x24
    jmp isr_common

.global isr_xhci0
isr_xhci0:
    push 0
    push 0x30
    jmp isr_common

.global isr_xhci1
isr_xhci1:
    push 0
    push 0x31
    jmp isr_common

.global isr_xhci2
isr_xhci2:
    push 0
    push 0x32
    jmp isr_common

.global isr_xhci3
isr_xhci3:
    push 0
    push 0x33
    jmp isr_common
