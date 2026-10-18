@ A stand-in GBA BIOS for the NanoBoyAdvance runner, which skips the boot
@ screen. The demo makes no BIOS calls, so all a BIOS does for it is pass
@ interrupts on: the IRQ handler below is the official BIOS's own sequence
@ (GBATEK, "BIOS Interrupt handling"), so an interrupt costs the same cycles
@ as on a GBA. Every other exception hangs.

    .text
    .arm
    b       hang                    @ 0x00 reset (the runner skips the boot screen)
    b       hang                    @ 0x04 undefined instruction
    b       hang                    @ 0x08 SWI
    b       hang                    @ 0x0C prefetch abort
    b       hang                    @ 0x10 data abort
    b       hang                    @ 0x14 reserved
    b       irq                     @ 0x18 IRQ
    b       hang                    @ 0x1C FIQ
hang:
    b       hang

    .org    0x128
irq:
    stmfd   sp!, {r0-r3, r12, lr}   @ save registers on the IRQ stack
    mov     r0, #0x04000000
    add     lr, pc, #0              @ return to the ldmfd
    ldr     pc, [r0, #-4]           @ the handler at 0x03007FFC (via its mirror 0x03FFFFFC)
    ldmfd   sp!, {r0-r3, r12, lr}
    subs    pc, lr, #4

    .org    0x4000
