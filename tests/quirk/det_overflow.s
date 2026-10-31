# quirk detection: overflow (trips overflow)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x7fff                           # 40000000: 3c087fff
    ori $t0,$t0,0xffff                       # 40000004: 3508ffff
    addi $t1,$t0,1                           # 40000008: 21090001
    break                                    # 4000000c: 0000000d
