# quirk detection: cop0 (trips cop0)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    mfc0 $t0,$15                             # 40000000: 40087800
    break                                    # 40000004: 0000000d
