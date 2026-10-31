#ifndef VV_GPU_H
#define VV_GPU_H
#include <stdint.h>
#include <unicorn/unicorn.h>
/* GraphicsProcessor model. Opcodes per hardware/src/gpu/gpcommands.vh:7-11. */
#define VV_GOP_STOP 0x0
#define VV_GOP_FILL 0x1
#define VV_GOP_LINE 0x2
#define VV_GOP_ELIP 0x3
#define VV_GOP_RECT 0x4
typedef struct {
    uint32_t launches, cmds, faults;
    uint32_t last_frame;   /* frame# drawn by last launch */
} vv_gpu_stats;
/* GP_CODE write: walk command list at code_addr, drawing into frame FRAME_BITS(gp_frame).
 * Completes synchronously. Returns the number of commands executed; -2 bad opcode (GP_fault set),
 * -1 no STOP within the runaway guard or an unreadable address. */
int vv_gpu_run(uc_engine *uc, uint32_t gp_frame, uint32_t code_addr, int verbose);
const vv_gpu_stats *vv_gpu_get_stats(void);
#endif
