# quirk emulation: lwlr (lwlr on: ['t2=0', 't0=0x50000000']; off: ['t2=0x22334477'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    lui $t1,0x1122                           # 40000004: 3c091122
    ori $t1,$t1,0x3344                       # 40000008: 35293344
    sw $t1,0($t0)                            # 4000000c: ad090000
    addiu $t2,$zero,119                      # 40000010: 240a0077
    lwl $t2,1($t0)                           # 40000014: 890a0001
    nop                                      # 40000018: 00000000
    break                                    # 4000001c: 0000000d
