# quirk emulation: load_use (load-use on: ['t3=1', 't2=0x99']; off: ['t3=0x99', 't2=0x99'])
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    addiu $t1,$zero,153                      # 40000004: 24090099
    sw $t1,0($t0)                            # 40000008: ad090000
    addiu $t2,$zero,1                        # 4000000c: 240a0001
    lw $t2,0($t0)                            # 40000010: 8d0a0000
    addu $t3,$t2,$zero                       # 40000014: 01405821
    break                                    # 40000018: 0000000d
