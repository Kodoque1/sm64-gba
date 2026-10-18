@ VBlank interrupt handler. The BIOS dispatcher saves r0-r3, r12 and lr and
@ jumps here in ARM state, so this must be ARM code. It acknowledges the
@ interrupt (IF and the BIOS mirror at 0x03007FF8) and counts VBlanks.

    .section .iwram, "ax"
    .arm
    .align 2
    .global irq_handler
irq_handler:
    mov     r0, #0x04000000
    ldr     r1, [r0, #0x200]        @ IE | IF << 16
    and     r1, r1, r1, lsr #16     @ IE & IF
    add     r3, r0, #0x200
    strh    r1, [r3, #2]            @ acknowledge in IF
    ldr     r2, =0x03007FF8
    ldrh    r3, [r2]
    orr     r3, r3, r1
    strh    r3, [r2]                @ BIOS IF, for IntrWait
    tst     r1, #1
    ldrne   r2, =gVBlankCount
    ldrne   r3, [r2]
    addne   r3, r3, #1
    strne   r3, [r2]
    bx      lr
    .pool

@ void fill_words(u32 *dst, u32 value, s32 count)
@ Fills count words with stmia bursts of 8 (about 2 cycles per word into
@ VRAM, versus ~8 for a C store loop). ARM code in IWRAM.
    .section .iwram, "ax"
    .arm
    .align 2
    .global fill_words
fill_words:
    cmp     r2, #0
    bxle    lr
    push    {r4-r9}
    mov     r3, r1
    mov     r4, r1
    mov     r5, r1
    mov     r6, r1
    mov     r7, r1
    mov     r8, r1
    mov     r9, r1
    subs    r2, r2, #8
    blt     2f
1:  stmia   r0!, {r1, r3-r9}
    subs    r2, r2, #8
    bge     1b
2:  adds    r2, r2, #8
    beq     3f
4:  str     r1, [r0], #4
    subs    r2, r2, #1
    bne     4b
3:  pop     {r4-r9}
    bx      lr
