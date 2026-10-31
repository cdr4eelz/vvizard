# quirk detection: lwl (trips lwlr)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    lwl $t1,1($t0)                           # 40000004: 89090001
    nop                                      # 40000008: 00000000
    break                                    # 4000000c: 0000000d
