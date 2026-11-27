; The Ark OS kernel, pure assembly version. For now it only prints
; "hello world" on the serial port.
;
; The boot sector loads this at 0x10000, already in 64-bit mode, and calls
; kernel_start. kernel.c is the C version of the same kernel.

[bits 64]

COM1 equ 0x3F8                  ; The first serial port. QEMU prints what we send.

; kernel.ld puts .text.start first, at 0x10000, where the boot sector jumps.
section .text.start progbits alloc exec nowrite align=16

global kernel_start
kernel_start:
    mov rsi, message            ; rsi = address of the first letter
    mov dx, COM1                ; dx = the serial port
.next:
    mov al, [rsi]               ; al = the letter rsi points to
    cmp al, 0                   ; Is it the 0 at the end?
    je .halt                    ; Yes: we're done.
    out dx, al                  ; No: send it...
    inc rsi                     ; ...and move to the next letter.
    jmp .next
.halt:
    cli                         ; No interrupts...
    hlt                         ; ...and stop the CPU.
    jmp .halt                   ; If something wakes it anyway, stop again.

section .rodata

; 13, 10 = go to a new line. 0 = the end.
message: db "hello world", 13, 10, 0
