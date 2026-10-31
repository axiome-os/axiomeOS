OUTPUT_FORMAT("elf64-x86-64")

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R | X */
    data    PT_LOAD FLAGS(6);   /* R | W */
    dynamic PT_DYNAMIC FLAGS(6);/* R | W */
}

SECTIONS
{
    . = 0x18000000000;

    .text : {
        *(.text*)
        *(.text)
        *(.plt)
        *(.plt.got)
        *(.plt.sec)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
        *(.rodata)
        *(.eh_frame*)
    } :text

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
        *(.sdata*)
    } :data
    .rela.dyn  : { *(.rela.dyn)  } :data
    .rela.plt  : { *(.rela.plt)  } :data
    .got       : { *(.got*)      } :data
    .dynsym    : { *(.dynsym)    } :data
    .dynstr    : { *(.dynstr)    } :data
    .hash      : { *(.hash)      } :data
    .gnu.hash  : { *(.gnu.hash)  } :data
    .gnu.version     : { *(.gnu.version) }     :data
    .gnu.version_d   : { *(.gnu.version_d) }   :data
    .gnu.version_r   : { *(.gnu.version_r) }   :data
    .dynamic   : { *(.dynamic)   } :dynamic :data

    . = ALIGN(0x1000);
    .bss : {
        *(.bss*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.note*)
        *(.comment)
    }
}