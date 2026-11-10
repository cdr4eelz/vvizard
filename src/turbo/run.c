#include "turbo.h"
#include "hw.h"
#include "display.h"
#include "shmem.h"
#include "term.h"
#include <stdio.h>
#include <unistd.h>

static void publish(turbo *t, int final) {
    static uint64_t last;
    if (vv_disp_turbo_stop()) atomic_store_explicit(&t->stop, 1, memory_order_relaxed);
    uint64_t now = vv_mono_ns();
    if (!final && now - last < 16579200) return;
    last = now;
    /* Status text 4 times a second like the other CPUs (display.c STATUS_HZ): re-formatting it at
     * every publish made the numbers flicker unreadably. */
    static char l1[160], l2[160]; static uint64_t st_at;
    if (final || !st_at || now - st_at >= 250000000u) {
        char smtext[96]; vv_sm sm;
        vv_sm_read(turbo_sm_read, t, VV_SM_BASE_ADDR, &sm);
        vv_sm_status(&sm, VV_SM_BASE_ADDR, smtext, sizeof smtext);
        double secs = (now - t->t0) * 1e-9;
        snprintf(l1, sizeof l1, "turbo C %.2fM/s I - F %.1f/s", secs > 0 ? t->insns / secs * 1e-6 : 0, secs > 0 ? t->pf_frames / secs : 0);
        snprintf(l2, sizeof l2, "%s pc %08x", smtext, t->pc);
        st_at = now;
    }
    vv_disp_turbo_publish(t->ram[1], &t->scan, t->lcd, l1, l2);
}
void turbo_publish(turbo *t) { publish(t, 0); }
static void *cpu_worker(void *arg) {
    turbo *t = arg; turbo_cpu(t);
    if (t->window) { publish(t, 1); vv_disp_turbo_done(); }   /* final frame + status */
    return NULL;
}
static int dump_frame(turbo *t, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n800 600\n255\n");
    for (unsigned y = 0; y < 600; y++) for (unsigned x = 0; x < 800; x++) {
        uint32_t v = turbo_word(t->ram[1] + (t->pf_cur << VV_FSHIFT) + y * VV_ROW_PITCH + x * 4);
        uint8_t rgb[] = { v >> 16, v >> 8, v }; fwrite(rgb, 1, 3, f);
    }
    return fclose(f) ? -1 : 0;
}
int turbo_run(turbo *t, const turbo_opts *o) {
    t->pc = o->entry; t->gpr[29] = o->sp; t->max_insns = o->max_insns; t->max_ns = o->max_ns; t->mhz = o->mhz;
    t->exit_on_halt = o->exit_on_halt;
    vv_disp_init(o->window); t->window = vv_disp_has_window();
    if (turbo_gpu_start(t)) { vv_eprintf("[turbo] cannot start GPU worker\n"); vv_disp_close(); return 1; }
    if (t->mhz > 0)
        vv_eprintf("[run] cpu=turbo big-endian entry=0x%08x max-insns=%llu mhz=%.3g (fixed MIPS150, CPU + GPU%s)\n",
            t->pc, (unsigned long long)t->max_insns, t->mhz, t->window ? " + display" : "");
    else
        vv_eprintf("[run] cpu=turbo big-endian entry=0x%08x max-insns=%llu (fixed MIPS150, CPU + GPU%s)\n",
            t->pc, (unsigned long long)t->max_insns, t->window ? " + display" : "");
    t->t0 = vv_mono_ns();
    if (t->window) {
        pthread_t cpu;
        vv_disp_threaded = 1;
        if (pthread_create(&cpu, NULL, cpu_worker, t)) {
            vv_disp_threaded = 0; turbo_gpu_stop(t); vv_disp_close(); return 1;
        }
        vv_disp_main_loop(); pthread_join(cpu, NULL); vv_disp_threaded = 0;
    } else turbo_cpu(t);
    uint64_t elapsed = vv_mono_ns() - t->t0;
    turbo_gpu_stop(t); /* shutdown only: not a guest-visible ready-edge wait */
    vv_eprintf("[turbo] insns %llu cycles %llu in %.6f s: %.3f Mcyc/s; PF %llu IRQ %llu GPU %llu dropped %llu\n",
        (unsigned long long)t->insns, (unsigned long long)t->insns, elapsed * 1e-9, elapsed ? t->insns * 1e3 / elapsed : 0,
        (unsigned long long)t->pf_frames, (unsigned long long)t->irqs, (unsigned long long)t->launches, (unsigned long long)t->drops);
    if (t->halt_stores) vv_eprintf("[halt] HaltVal = 0x%08x (%u), last stored at pc=0x%08x (%llu stores)\n", t->halt, t->halt, t->halt_pc, (unsigned long long)t->halt_stores);
    if (o->dump_lcd) for (unsigned r = 0; r < 2; r++) {
        char text[17], hex[49];
        for (unsigned c = 0; c < 16; c++) {
            uint8_t v = t->lcd[r * 16 + c]; text[c] = v >= 32 && v < 127 ? v : '.';
            snprintf(hex + 3 * c, 4, " %02x", v);
        }
        text[16] = 0; vv_eprintf("[lcd] %u |%s| hex%s\n", r + 1, text, hex);
    }
    vv_sm sm; vv_sm_read(turbo_sm_read, t, VV_SM_BASE_ADDR, &sm);
    if (o->dump_sm) vv_sm_dump(&sm, VV_SM_BASE_ADDR); else vv_sm_warn(&sm, VV_SM_BASE_ADDR);
    int rc = t->error;
    if (o->dump_frame && dump_frame(t, o->dump_frame)) rc = 1;
    vv_disp_close();
    return rc;
}
