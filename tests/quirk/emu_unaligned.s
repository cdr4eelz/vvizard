# quirk emulation: unaligned (unaligned on: ['t2=0', 't4=0x11223344', 't0=0x50000000']; off: exc)
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
    addiu $t2,$zero,7                        # 40000010: 240a0007
    lw $t2,1($t0)                            # 40000014: 8d0a0001
    addiu $t3,$zero,153                      # 40000018: 240b0099
    sw $t3,2($t0)                            # 4000001c: ad0b0002
    lw $t4,0($t0)                            # 40000020: 8d0c0000
    nop                                      # 40000024: 00000000
    break                                    # 40000028: 0000000d
