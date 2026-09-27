#include <platform.h>
#include <asm/boot.h>
OUTPUT_ARCH(riscv)
ENTRY(_entry)
SECTIONS {
    . = LOAD;
    kernel_text_start = .;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    . = ALIGN(4096);
    kernel_text_end = .;
    kernel_rodata_start = .;
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    kernel_rodata_end = .;
    kernel_data_start = .;
    .data : { PROVIDE(__global_pointer$ = . + 0x800); *(.sdata .sdata.* .data .data.*) }
    . = ALIGN(8);
    __bss_start = .;
    .bss : { *(.sbss .sbss.* .bss .bss.*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
    .stacks (NOLOAD) : ALIGN(16) { boot_stacks = .; . += NCPU * STACK_SIZE; }
    . = ALIGN(4096);
    kernel_end = .;
    ASSERT((boot_stacks % 16) == 0, "stack alignment")
    ASSERT((kernel_text_start % 4096) == 0, "text alignment")
    ASSERT((kernel_text_end % 4096) == 0, "text end alignment")
    ASSERT((kernel_rodata_start % 4096) == 0, "rodata alignment")
    ASSERT((kernel_rodata_end % 4096) == 0, "rodata end alignment")
    ASSERT((kernel_data_start % 4096) == 0, "data alignment")
    ASSERT((kernel_end % 4096) == 0, "kernel end alignment")
    /DISCARD/ : { *(.eh_frame .comment) }
}
