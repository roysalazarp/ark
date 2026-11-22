; The boot sector: the first 512 bytes of the disk.
;
; The BIOS copies it to address 0x7C00 and jumps to it, with the CPU in
; 16-bit "real mode". Its jobs:
;   1. Load the kernel from the disk into memory at KERNEL_ADDRESS.
;   2. Ask the BIOS for the memory map: which RAM is free.
;   3. Set up page tables, which 64-bit mode requires.
;   4. Switch the CPU straight from 16-bit mode to 64-bit "long mode".
;   5. Jump to the kernel.
;
; The build passes KERNEL_SECTORS: how many 512-byte sectors the kernel uses.
; The kernel starts on the disk right after this sector.

KERNEL_ADDRESS equ 0x10000      ; Where the kernel goes. Must match kernel.ld.
KERNEL_SEGMENT equ KERNEL_ADDRESS >> 4
SECTORS_PER_READ equ 64         ; 32 KiB per read. Some BIOSes can't do more.

; Page tables. Each one is a 4 KiB table of 512 entries, in free low memory.
PML4_ADDRESS equ 0x1000         ; Level 4: one entry, pointing to the PDPT.
PDPT_ADDRESS equ 0x2000         ; Level 3: one entry, pointing to the PD.
PD_ADDRESS   equ 0x3000         ; Level 2: 512 entries of 2 MiB = the first 1 GiB.

; The memory map from the BIOS goes here, for the kernel to read.
; Must match MEMORY_MAP_ADDRESS in kernel.c. Layout:
;   0x4000: 2-byte count of entries
;   0x4008: the entries, 24 bytes each (start, length, type, extra flags)
MEMORY_MAP_ADDRESS equ 0x4000
MEMORY_MAP_ENTRIES equ MEMORY_MAP_ADDRESS + 8
MEMORY_MAP_MAX     equ 128      ; 128 * 24 bytes fits below the stack.
E820_ENTRY_SIZE    equ 24
E820_MAGIC         equ 0x534D4150 ; "SMAP" in ASCII. The BIOS wants it in edx.

PAGE_PRESENT  equ 1 << 0
PAGE_WRITABLE equ 1 << 1
PAGE_HUGE     equ 1 << 7        ; In the PD: the entry is a whole 2 MiB page.

[bits 16]
[org 0x7C00]

start:
    ; The BIOS leaves the segment registers in an unknown state. Zero them,
    ; and put the stack just below this code (it grows down).
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; The BIOS tells us which disk we booted from in dl. Keep it for reading.
    mov [boot_drive], dl

; --- 1. Load the kernel ------------------------------------------------------

    ; Ask the BIOS (interrupt 0x13, function 0x42) to read sectors. It reads
    ; what the "disk address packet" (dap below) describes. We read in chunks,
    ; moving the destination segment forward after each one.
    mov cx, KERNEL_SECTORS
.read_chunk:
    mov ax, SECTORS_PER_READ
    cmp cx, ax
    jae .read
    mov ax, cx                  ; Last chunk: only what is left.
.read:
    mov [dap.count], ax
    mov si, dap
    mov dl, [boot_drive]
    push ax
    mov ah, 0x42
    int 0x13
    pop ax
    jc disk_error               ; The BIOS sets the carry flag on failure.

    add [dap.lba], ax           ; Next sector on the disk.
    shl ax, 5                   ; Sectors -> segments: 512 bytes = 32 * 16.
    add [dap.segment], ax       ; Next place in memory.
    sub cx, [dap.count]
    jnz .read_chunk

; --- 2. Memory map -----------------------------------------------------------

    ; Ask the BIOS (interrupt 0x15, function 0xE820) which memory is free RAM.
    ; Each call writes one entry at es:di. ebx says where to continue next
    ; time; the BIOS sets it to 0 after the last entry.
    mov di, MEMORY_MAP_ENTRIES
    xor ebx, ebx                ; 0 = start from the first entry.
    xor bp, bp                  ; bp counts the entries we kept.
.next_entry:
    mov eax, 0xE820
    mov edx, E820_MAGIC
    mov ecx, E820_ENTRY_SIZE
    mov dword [di + 20], 1      ; Default for the extra flags: "valid".
    int 0x15
    jc .map_done                ; Carry set: no more entries (or no support).
    cmp eax, E820_MAGIC         ; A working BIOS echoes the magic back.
    jne .map_done
    add di, E820_ENTRY_SIZE     ; Keep this entry.
    inc bp
    cmp bp, MEMORY_MAP_MAX
    je .map_done
    test ebx, ebx
    jnz .next_entry
.map_done:
    mov [MEMORY_MAP_ADDRESS], bp

; --- 3. Page tables ----------------------------------------------------------

    ; In 64-bit mode every address goes through page tables. We map the first
    ; 1 GiB to itself ("identity map"), so address X still means memory at X.

    ; Clear the three tables (3 * 4 KiB) first.
    cld
    mov di, PML4_ADDRESS
    xor ax, ax
    mov cx, 3 * 4096 / 2
    rep stosw

    mov word [PML4_ADDRESS], PDPT_ADDRESS | PAGE_PRESENT | PAGE_WRITABLE
    mov word [PDPT_ADDRESS], PD_ADDRESS | PAGE_PRESENT | PAGE_WRITABLE

    ; Fill the PD: entry i maps 2 MiB at address i * 2 MiB.
    ; Each entry is 8 bytes. The address is in bits 21 and up, so in 16-bit
    ; pieces we only write the low half (flags) and the next half (address).
    mov di, PD_ADDRESS
    mov cx, 512
    xor bx, bx                  ; bx = address >> 16, goes up by 0x20 (2 MiB).
.fill_pd:
    mov word [di], PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE
    mov [di + 2], bx
    add bx, 0x20
    add di, 8
    loop .fill_pd

; --- 4. Switch to 64-bit mode ------------------------------------------------

    cli                         ; No interrupts: we have no handlers for them.


    ; Turn on the "A20 line". Without it, address bit 20 is always 0
    ; (an old PC compatibility trick). This is the fast way, via port 0x92.
    in al, 0x92
    or al, 2
    out 0x92, al

    lgdt [gdt.pointer]          ; Tell the CPU where our segment table is.

    mov eax, 1 << 5             ; CR4.PAE: bigger page table entries. Required.
    mov cr4, eax

    mov eax, PML4_ADDRESS       ; CR3: where the top page table is.
    mov cr3, eax

    mov ecx, 0xC0000080         ; EFER, a model-specific register.
    rdmsr
    or eax, 1 << 8              ; EFER.LME: allow long mode.
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 0); CR0.PG (paging) + CR0.PE (protection).
    mov cr0, eax                ; Long mode is now on...

    jmp gdt.code:long_mode      ; ...and this jump loads the 64-bit code segment.

disk_error:
    mov si, disk_error_message
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E                ; BIOS: print one character on the screen.
    int 0x10
    jmp .print
.halt:
    hlt
    jmp .halt

; --- 5. Jump to the kernel ---------------------------------------------------

[bits 64]
long_mode:
    ; Data segment registers aren't used in 64-bit mode, but zero them anyway.
    xor eax, eax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, 0x7C00             ; Same stack area as before.
    mov rax, KERNEL_ADDRESS
    call rax                    ; The kernel should never return...
.halt:
    hlt                         ; ...but stop here if it does.
    jmp .halt

; --- Data --------------------------------------------------------------------

boot_drive: db 0

disk_error_message: db "Disk read error", 0

; Disk address packet for the BIOS read.
align 4
dap:
    db 16                       ; Size of this packet.
    db 0
.count:   dw 0                  ; Sectors to read.
.offset:  dw 0                  ; Destination: segment:offset.
.segment: dw KERNEL_SEGMENT
.lba:     dq 1                  ; First sector to read (0 is this one).

; The GDT (global descriptor table). 64-bit mode mostly ignores segments,
; but the CPU still needs one code segment marked as 64-bit.
align 8
gdt:
    dq 0                        ; Entry 0 must be empty.
.code: equ $ - gdt
    ; Present, ring 0, code, executable, and the "L" bit (64-bit).
    dq (1 << 47) | (1 << 44) | (1 << 43) | (1 << 53)
.pointer:
    dw $ - gdt - 1              ; Size of the table minus 1.
    dd gdt                      ; Its address.

; Pad to 510 bytes, then the 2-byte signature the BIOS looks for.
times 510 - ($ - $$) db 0
dw 0xAA55
