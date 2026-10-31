# gplaunch.s - GP_FRAME=1, GP_CODE=0x12003000 (tests/gpcmds.s), PF_FRAME=1 (test-hex "gp")
# vv-load: 0x40000000
# vv-isa: -mips1
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x8000                           # 40000000: 3c088000
    addiu $t1,$zero,1                        # 40000004: 24090001
    sw $t1,84($t0)                           # 40000008: ad090054
    lui $t2,0x1200                           # 4000000c: 3c0a1200
    ori $t2,$t2,0x3000                       # 40000010: 354a3000
    sw $t2,88($t0)                           # 40000014: ad0a0058
    sw $t1,80($t0)                           # 40000018: ad090050
# PF_FRAME takes effect at the end of the frame being scanned: wait until it reads back frame 1
L_wait:
    lw $t6,80($t0)
    nop
    srl $t6,$t6,22
    andi $t6,$t6,0x3f
    addiu $t7,$zero,1
    bne $t6,$t7,L_wait
    nop
    break                                    # 4000001c: 0000000d
