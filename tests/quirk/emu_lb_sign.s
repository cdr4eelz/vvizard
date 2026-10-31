# quirk emulation: lb_sign (lb-sign on: ['t2=0x80', 't3=0x8001']; off: ['t2=0xffffff80', 't3=0xffff8001'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    addiu $t1,$zero,128                      # 40000004: 24090080
    sb $t1,0($t0)                            # 40000008: a1090000
    ori $t1,$zero,0x8001                     # 4000000c: 34098001
    sh $t1,2($t0)                            # 40000010: a5090002
    lb $t2,0($t0)                            # 40000014: 810a0000
    lh $t3,2($t0)                            # 40000018: 850b0002
    nop                                      # 4000001c: 00000000
    break                                    # 40000020: 0000000d
