/* Optimized GBA layout. Same as gba.ld, plus hot code moved to IWRAM
 * (32-bit bus, no wait states):
 *   - objects named *_iw.o (our ARM-mode builds of hot decomp files)
 *   - libgcc's single-precision soft-float routines (hand-written ARM code,
 *     which runs at half speed or worse from the 16-bit cartridge bus)
 *   - newlib's sqrtf (ARM code, bit-by-bit square root: ~4% of Mario's time
 *     when run from cartridge ROM). */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
}

__sp_irq = 0x03007FA0;
__sp_usr = 0x03007F00;

SECTIONS {
    .text : {
        KEEP(*(.crt0))
        *(EXCLUDE_FILE(*_iw.o *libgcc.a:_arm_addsubsf3.o *libgcc.a:_arm_muldivsf3.o *libgcc.a:_arm_cmpsf2.o *libgcc.a:_arm_fixsfsi.o *libgcc.a:_arm_unordsf2.o *libm.a:libm_a-ef_sqrt.o *libm.a:libm_a-wf_sqrt.o) .text EXCLUDE_FILE(*_iw.o *libgcc.a:_arm_addsubsf3.o *libgcc.a:_arm_muldivsf3.o *libgcc.a:_arm_cmpsf2.o *libgcc.a:_arm_fixsfsi.o *libgcc.a:_arm_unordsf2.o *libm.a:libm_a-ef_sqrt.o *libm.a:libm_a-wf_sqrt.o) .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
    } > rom

    .rodata : ALIGN(4) {
        *(EXCLUDE_FILE(*_iw.o) .rodata EXCLUDE_FILE(*_iw.o) .rodata.*)
    } > rom

    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > rom

    .iwram : ALIGN(4) {
        __iwram_start = .;
        *(.iwram .iwram.*)
        *_iw.o(.text .text.* .rodata .rodata.*)
        *libgcc.a:_arm_addsubsf3.o(.text .text.*)
        *libgcc.a:_arm_muldivsf3.o(.text .text.*)
        *libgcc.a:_arm_cmpsf2.o(.text .text.*)
        *libgcc.a:_arm_fixsfsi.o(.text .text.*)
        *libgcc.a:_arm_unordsf2.o(.text .text.*)
        *libm.a:libm_a-ef_sqrt.o(.text .text.*)
        *libm.a:libm_a-wf_sqrt.o(.text .text.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT > rom
    __iwram_lma = LOADADDR(.iwram);

    .data : ALIGN(4) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > ewram AT > rom
    __data_lma = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > ewram

    end = .;
    __end__ = .;
    __ewram_used = . - ORIGIN(ewram);

    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
