# quirk detection: reserved (trips reserved)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    movz $t2,$a0,$zero                       # 40000000: 0080500a
    break                                    # 40000004: 0000000d
