OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x1000;
    .text : { *(.text .text.*) *(.rodata .rodata.*) }
    ASSERT(SIZEOF(.text) <= 4096, "user image exceeds one code page")
    /DISCARD/ : { *(.eh_frame .comment .riscv.attributes) }
}
