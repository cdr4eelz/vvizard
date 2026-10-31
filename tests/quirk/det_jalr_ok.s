# quirk detection: jalr_ok (clean: must not trip any quirk)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x4000                           # 40000000: 3c084000
    ori $t0,$t0,0x10                         # 40000004: 35080010
    jalr $t0                                 # 40000008: 0100f809
    nop                                      # 4000000c: 00000000
    nop                                      # 40000010: 00000000
    nop                                      # 40000014: 00000000
    break                                    # 40000018: 0000000d
