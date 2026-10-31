# loop.s - counted loop (test-hex "loop": t0=30, t2=0x11e)
# vv-load: 0x40000000
# vv-isa: -mips1
.set noreorder
.set noat
.text
.global _start
_start:
    addiu $t0,$zero,0                        # 40000000: 24080000
    addiu $t1,$zero,10                       # 40000004: 2409000a
L_40000008:
    addiu $t0,$t0,3                          # 40000008: 25080003
    addiu $t1,$t1,-1                         # 4000000c: 2529ffff
    bne $t1,$zero,L_40000008                 # 40000010: 1520fffd
    nop                                      # 40000014: 00000000
    ori $t2,$t0,0x100                        # 40000018: 350a0100
    break                                    # 4000001c: 0000000d
