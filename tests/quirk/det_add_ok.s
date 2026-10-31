# quirk detection: add_ok (clean: must not trip any quirk)
# vv-load: 0x40000000
# vv-isa: -mips32r2
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $t0,$zero,5                        # 40000000: 24080005
    add $t1,$t0,$t0                          # 40000004: 01084820
    sub $t1,$t0,$t0                          # 40000008: 01084822
    addiu $t2,$t0,-9                         # 4000000c: 250afff7
    break                                    # 40000010: 0000000d
