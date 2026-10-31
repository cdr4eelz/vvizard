# guest_exit.s - guest exit register (vvizard-only, 0x80000084: first word of the 0x84-0xFC extended
# area past RANDOM32; MemMapIO.v decodes nothing there, so on the FPGA the store is a no-op).
# Stores exit code 42, then does what FPGA code does next: interrupts off (Status IE = 0) and spin.
# vvizard stops right after the store (exit status 42, $s0 still 0); with --no-guest-exit it goes on
# into the loop ($s0 = 1) until --max-insns.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    move  $s0, $zero
    li    $t1, 42
    sw    $t1, VV_EXIT($t0)      # vvizard stops here
    li    $s0, 1                 # FPGA (or --no-guest-exit): carry on
    mfc0  $t2, $12
    nop
    li    $t3, ~1
    and   $t2, $t2, $t3
    mtc0  $t2, $12               # Status.IE = 0: no interrupts
1:  b     1b                     # spin forever
    nop
