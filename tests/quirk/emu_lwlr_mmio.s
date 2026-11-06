# quirk emulation: lwlr MMIO side effect (lwl of UARX_DATA pops RX even though rt gets 0)
# unicorn+fast with +lwlr and --uart-in A: after wait for VALID, lwl RX_DATA -> t2=0 and VALID cleared
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui   $t0, 0x8000
1:  lw    $t1, 4($t0)            # RX_VALID
    nop
    beq   $t1, $zero, 1b
    nop
    lwl   $t2, 12($t0)           # RX_DATA: lwlr writes 0, must still side-read/pop
    nop
    lw    $t3, 4($t0)            # VALID must be 0 if pop happened
    nop
    break
