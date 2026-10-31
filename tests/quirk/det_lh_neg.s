# quirk detection: lh_neg (trips lb-sign)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    ori $t1,$zero,0x8001                     # 40000004: 34098001
    sh $t1,2($t0)                            # 40000008: a5090002
    nop                                      # 4000000c: 00000000
    lh $t3,2($t0)                            # 40000010: 850b0002
    nop                                      # 40000014: 00000000
    break                                    # 40000018: 0000000d
