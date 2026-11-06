/* decode.h - one predecoded guest instruction.  Quirk choices that do not depend on register
 * values are baked into `op` when the block is built. */
#ifndef VV_FAST_DECODE_H
#define VV_FAST_DECODE_H
#include <stdint.h>

enum {
    OP_NOP = 0,
    OP_ALU,          /* rd = alu(rs, rt) or shift; imm holds the alu id, sa in alu for shifts */
    OP_IMM,          /* rt = alu(rs, imm); imm is already sign- or zero-extended; alu id in .alu */
    OP_LUI,
    OP_B,            /* conditional branch; .alu is the condition, .imm is the offset, flags link/likely */
    OP_J,            /* j/jal; .imm is the target, flags link */
    OP_JR,           /* jr/jalr */
    OP_MEM,          /* load/store; .alu = size (1/2/4) or 0 for lwl/lwr/swl/swr; flags say which */
    OP_MFC0,
    OP_MTC0,
    OP_ERET,
    OP_SYSCALL,
    OP_BREAK,
    OP_HILO,         /* mfhi/mflo/mthi/mtlo/mult/div; which in .alu */
    OP_MOVZ,         /* real movz/movn when reserved is off; .alu 0 = movz, 1 = movn */
    OP_RD0,          /* reserved SPECIAL: rd = 0 (or the real op when in a delay slot) */
    OP_TRAP,         /* regimm trap, reserved off */
    OP_RI,           /* reserved instruction, reserved quirk off: standard MIPS exception */
    OP_RNOP,         /* reserved opcode the MIPS150 treats as a nop (still counted) */
    OP_COP0X,        /* COP0 other than mfc0/mtc0/eret: nop, or RI in a delay slot */
    OP_FETCH0,       /* hardware fetch of 0 (unmapped-fetch / MMIO fetch) */
    OP_FETCHX        /* fetch fault when unmapped-fetch is off */
};

enum { /* ALU / IMM ids */
    A_ADD, A_ADDU, A_SUB, A_SUBU, A_AND, A_OR, A_XOR, A_NOR,
    A_SLT, A_SLTU, A_SLL, A_SRL, A_SRA, A_SLLV, A_SRLV, A_SRAV,
    A_ROTR, A_ROTRV
};
enum { /* branch conditions in .alu */
    B_EQ, B_NE, B_LEZ, B_GTZ, B_LTZ, B_GEZ, B_REGIMM
};
enum { /* HILO .alu */
    H_MFHI, H_MFLO, H_MTHI, H_MTLO, H_MULT, H_MULTU, H_DIV, H_DIVU
};
enum {
    FL_LINK   = 1,
    FL_LIKELY = 2,
    FL_STORE  = 4,
    FL_SIGN   = 8,
    FL_LWL    = 16,
    FL_LWR    = 32,
    FL_SWL    = 64,
    FL_SWR    = 128,
    FL_ADDR0  = 256
};

typedef struct {
    uint32_t insn, pc;
    uint16_t op, flags;
    uint8_t rs, rt, rd, sa, alu;
    int32_t imm;
} Dec;

void vv_decode_at(uint32_t pc, Dec *d);
#endif
