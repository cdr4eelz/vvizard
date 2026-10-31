# quirk detection: lb_pos (clean: must not trip any quirk)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    addiu $t1,$zero,127                      # 40000004: 2409007f
    sb $t1,0($t0)                            # 40000008: a1090000
    nop                                      # 4000000c: 00000000
    lb $t2,0($t0)                            # 40000010: 810a0000
    nop                                      # 40000014: 00000000
    addiu $t1,$zero,32767                    # 40000018: 24097fff
    sh $t1,2($t0)                            # 4000001c: a5090002
    lh $t3,2($t0)                            # 40000020: 850b0002
    nop                                      # 40000024: 00000000
    lbu $t4,0($t0)                           # 40000028: 910c0000
    nop                                      # 4000002c: 00000000
    break                                    # 40000030: 0000000d
