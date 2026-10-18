@ Minimal GBA startup: ROM header placeholder, stacks, .data/.iwram copy,
@ .bss clear, then main(). The header is patched by tools/fix_gba_header.py
@ (title, fixed byte, complement checksum). No Nintendo logo is included:
@ emulators booting with skipBios don't check it, and for the demo ROMs
@ devkitPro's gbafix writes it (see the Makefile).

    .section .crt0, "ax"
    .arm
    .global _start
_start:
    b       reset
    .fill   188, 1, 0               @ header: logo, title, codes, checksum

reset:
    mov     r0, #0x12               @ IRQ mode
    msr     cpsr_c, r0
    ldr     sp, =__sp_irq
    mov     r0, #0x1F               @ System mode
    msr     cpsr_c, r0
    ldr     sp, =__sp_usr

    ldr     r0, =__iwram_lma        @ copy .iwram (fast code/data)
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
    bl      copy_words

    ldr     r0, =__data_lma         @ copy .data
    ldr     r1, =__data_start
    ldr     r2, =__data_end
    bl      copy_words

    ldr     r1, =__bss_start        @ clear .bss
    ldr     r2, =__bss_end
    mov     r0, #0
1:  cmp     r1, r2
    strlo   r0, [r1], #4
    blo     1b

    ldr     r0, =main
    mov     lr, pc
    bx      r0
2:  b       2b

copy_words:                         @ r0=src r1=dst r2=dst_end
    cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     copy_words
    bx      lr

    .pool
