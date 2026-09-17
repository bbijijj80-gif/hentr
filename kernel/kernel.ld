ENTRY(kernel_entry)

SECTIONS
{
    . = 0x200000;

    .text : {
        *(.text.start)
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
