# reset_regs.s - registers at reset (RegFile.v: no reset, R0 only): vvizard starts them at 0 and
# tracks them as never written.  Reading $sp before writing it is the undef-reg guest bug (run
# without --sp; with --sp the harness presets it).  $s0 = $sp as read.
# vv-load: 0x40000000
# vv-isa: -mips32r2
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    move  $s0, $sp               # undef-reg (unless --sp)
    addiu $sp, $zero, 16
    move  $s1, $sp               # written now: no report
    EXPECT $s1, 16
    break
