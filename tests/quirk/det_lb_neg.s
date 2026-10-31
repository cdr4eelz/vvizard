# quirk detection: lb_neg (trips lb-sign)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    addiu $t1,$zero,128                      # 40000004: 24090080
    sb $t1,0($t0)                            # 40000008: a1090000
    nop                                      # 4000000c: 00000000
    lb $t2,0($t0)                            # 40000010: 810a0000
    nop                                      # 40000014: 00000000
    nop                                      # 40000018: 00000000
    break                                    # 4000001c: 0000000d
