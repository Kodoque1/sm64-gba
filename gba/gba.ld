/* GBA memory map: code + const data in cartridge ROM, mutable data in
 * 256 KB EWRAM, stacks (and later hot code) in 32 KB IWRAM. */
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
        *(.text .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
    } > rom

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
    } > rom

    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > rom

    .iwram : ALIGN(4) {
        __iwram_start = .;
        *(.iwram .iwram.*)
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
