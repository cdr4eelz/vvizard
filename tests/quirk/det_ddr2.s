# quirk detection: ddr2 (trips ddr2)
# vv-load: 0x60000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x2000                           # 60000000: 3c082000
    sw $t1,256($t0)                          # 60000004: ad090100
    break                                    # 60000008: 0000000d
