# quirk emulation: bios_store (bios-store on: ['t2=0']; off: ['t2=0x1234'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x4000                           # 40000000: 3c084000
    addiu $t1,$zero,4660                     # 40000004: 24091234
    sw $t1,256($t0)                          # 40000008: ad090100
    lw $t2,256($t0)                          # 4000000c: 8d0a0100
    nop                                      # 40000010: 00000000
    break                                    # 40000014: 0000000d
