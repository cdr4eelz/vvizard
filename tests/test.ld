/* test.ld - link one test program at VV_LOAD (from the "# vv-load:" line of its .s). */
ENTRY(VV_LOAD)
SECTIONS
{
    . = VV_LOAD;
    .text : { *(.text) }        /* tests are .text only; an empty aligned .data would pad it */
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.gnu.attributes) *(.pdr) *(.comment) *(.note*) }
}
