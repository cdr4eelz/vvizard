# alias.s - DDR 0x1 vs 0x3 alias, before and after mtc0 Status (test-hex "alias": t3=t4=0xbeef)
# vv-load: 0xc0000180
# vv-isa: -mips1
.set noreorder
.set noat
.text
.global _start
_start:
    lui $t0,0x1200                           # c0000180: 3c081200
    ori $t1,$zero,0xbeef                     # c0000184: 3409beef
    sw $t1,12288($t0)                        # c0000188: ad093000
    lui $t2,0x3200                           # c000018c: 3c0a3200
    lw $t3,12288($t2)                        # c0000190: 8d4b3000
    mtc0 $zero,$12                           # c0000194: 40806000
    lw $t4,12288($t2)                        # c0000198: 8d4c3000
    break                                    # c000019c: 0000000d
