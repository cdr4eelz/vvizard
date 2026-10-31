# gpcmds.s - GraphicsProcessor command list (data, not CPU code) for the "gp" test.
# Loaded at 0x12003000 (graphics.h GPTEMP); tests/gplaunch.s points GP_CODE at it.
# Word formats per hardware/src/gpu/gpcommands.vh: opcode in 31:28, colour 27:0;
# points are {x[25:16], y[9:0]}; RECT/ELIP add a fill word (fill drawn iff fill[31:24] != 0).
# vv-load: 0x12003000
# vv-isa: -mips1
.text
.global _start
_start:
    .word 0x10000022                         # FILL  colour 0x000022
    .word 0x40ff0000, 0x00640064, 0x00c70095 # RECT  red edge, (100,100)-(199,149)
    .word 0xff00ff00                         #       fill 0x00ff00 (fill[31:24]=0xff: filled)
    .word 0x4fffffff, 0x012c0064, 0x015d0095 # RECT  white edge, (300,100)-(349,149)
    .word 0x0000ff00                         #       fill[31:24]=0: outline only
    .word 0x22ffff00, 0x00000000, 0x031f0257 # LINE  yellow, (0,0)-(799,599)
    .word 0x3000ffff, 0x01f40190, 0x0032001e # ELIP  cyan edge, centre (500,400), a=50 b=30
    .word 0xffff00ff                         #       fill magenta
    .word 0x00000000                         # STOP
