# frame
# vv-load: 0x40000000
# vv-isa: -mips1
.set noreorder
.set noat
.text
.global _start
_start:
    lui $s0,0x1040                           # 40000000: 3c101040
    lui $t9,0xff                             # 40000004: 3c1900ff
    ori $t9,$t9,0x8000                       # 40000008: 37398000
    addiu $t1,$zero,50                       # 4000000c: 24090032
L_40000010:
    sll $t2,$t1,0xc                          # 40000010: 00095300
    addu $t2,$t2,$s0                         # 40000014: 01505021
    addiu $t2,$t2,400                        # 40000018: 254a0190
    addiu $t3,$zero,32                       # 4000001c: 240b0020
L_40000020:
    sw $t9,0($t2)                            # 40000020: ad590000
    addiu $t2,$t2,4                          # 40000024: 254a0004
    addiu $t3,$t3,-1                         # 40000028: 256bffff
    bne $t3,$zero,L_40000020                 # 4000002c: 1560fffc
    nop                                      # 40000030: 00000000
    addiu $t1,$t1,1                          # 40000034: 25290001
    slti $t4,$t1,60                          # 40000038: 292c003c
    bne $t4,$zero,L_40000010                 # 4000003c: 1580fff4
    nop                                      # 40000040: 00000000
    lui $t0,0x8000                           # 40000044: 3c088000
    addiu $t5,$zero,1                        # 40000048: 240d0001
    sw $t5,80($t0)                           # 4000004c: ad0d0050
# PF_FRAME takes effect at the end of the frame being scanned: wait until it reads back frame 1
L_wait:
    lw $t6,80($t0)
    nop
    srl $t6,$t6,22
    andi $t6,$t6,0x3f
    addiu $t7,$zero,1
    bne $t6,$t7,L_wait
    nop
    break                                    # 40000050: 0000000d
