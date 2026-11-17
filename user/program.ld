OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x1000;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.* .srodata .srodata.*)
    }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    ASSERT(_start == 0x1000, "program entry must be at 0x1000")
    ASSERT(SIZEOF(.text) <= 4096, "program exceeds one code page")
    ASSERT(SIZEOF(.data) + SIZEOF(.bss) == 0, "program requires writable globals")
    /DISCARD/ : { *(.eh_frame .comment .riscv.attributes) }
}
