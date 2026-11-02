# mmio_ise.s - MemMapIO decode of the cs150 ISE branch (--mmio-decode=ise): only addra[5:0] is compared,
# so the registers repeat every 0x100.  0x80000184 is HaltVal (stored there, read at 0x80000084) and
# 0x80000100 is TX_READY (1, idle).  With the default lcd-text decode both are outside the registers
# (0x184: miss, reads 0; 0x100: LCD char 0, reads 0), so both EXPECTs fail there ($s5 = 2).
# vv-load: 0x40000000
# vv-isa: -mips1
.include "hw/hw.inc"
.set noreorder
.set noat
.text
.global _start
_start:
    HW_INIT
    li    $t1, 0x55
    sw    $t1, 0x184($t0)        # ISE: HaltVal alias
    lw    $t2, HALTVAL($t0)
    nop
    EXPECT $t2, 0x55             # 1
    lw    $t2, 0x100($t0)        # ISE: TX_READY alias
    nop
    EXPECT $t2, 1                # 2
    break
