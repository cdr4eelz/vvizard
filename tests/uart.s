# uart
# vv-load: 0x40000000
# vv-isa: -mips1
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x8000                           # 40000000: 3c088000
    addiu $t1,$zero,72                       # 40000004: 24090048
    sw $t1,8($t0)                            # 40000008: ad090008
    addiu $t1,$zero,105                      # 4000000c: 24090069
    sw $t1,264($t0)                          # 40000010: ad090108
    addiu $t1,$zero,10                       # 40000014: 2409000a
    sb $t1,11($t0)                           # 40000018: a109000b
    lw $t2,0($t0)                            # 4000001c: 8d0a0000
    break                                    # 40000020: 0000000d
