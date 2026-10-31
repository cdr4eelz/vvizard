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
/* Threaded GPU (docs/MULTITHREADING-PLAN.md section 6).  At a launch the emulator walks the list
 * without storing (vv_gpu_count: same messages and stats as vv_gpu_run; *pixels = pixels the
 * engines will store, *ncmds = commands incl. STOP) to get the completion cycle, then either hands
 * the drawing to the worker (vv_gpu_submit: stores into the DDR host buffer) or, when that is
 * unsafe, draws inline (vv_gpu_draw_uc).  vv_gpu_wait blocks until the worker finished its job. */
int  vv_gpu_count(uc_engine *uc, uint32_t gp_frame, uint32_t code_addr, int verbose, uint64_t *pixels, int *ncmds);
void vv_gpu_draw_uc(uc_engine *uc, uint32_t gp_frame, uint32_t code_addr, int ncmds);
int  vv_gpu_worker_start(void);
void vv_gpu_submit(uint32_t gp_frame, uint32_t code_addr, int ncmds);
int  vv_gpu_done(void);
void vv_gpu_wait(void);
void vv_gpu_worker_stop(void);
#endif
