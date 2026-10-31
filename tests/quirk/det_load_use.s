# quirk detection: load_use (trips load-use)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x5000                           # 40000000: 3c085000
    lw $t1,0($t0)                            # 40000004: 8d090000
    addu $t2,$t1,$t1                         # 40000008: 01295021
    break                                    # 4000000c: 0000000d
