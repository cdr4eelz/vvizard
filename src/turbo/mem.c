#include "turbo.h"
#include "term.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

int turbo_init(turbo *t) {
    memset(t, 0, sizeof *t);
    t->ram[1] = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t->ram[1] == MAP_FAILED) { t->ram[1] = NULL; return -1; }
    t->ram[2] = t->ram[3] = t->ram[1];
    t->mask[1] = t->mask[2] = t->mask[3] = 0xfffffff;
    unsigned bram[] = {4,5,6,12};
    for (unsigned i = 0; i < 4; i++) {
        unsigned k = bram[i]; t->ram[k] = calloc(1, 16384); t->mask[k] = 16383;
        if (!t->ram[k]) { turbo_destroy(t); return -1; }
    }
    t->compare = 0xffff; t->pf_at = 828960; t->event_at = 1;
    t->random = 0xace12468u; t->random_stale_at = UINT64_MAX; t->load_rt = -1; memset(t->lcd, ' ', sizeof t->lcd);
    return 0;
}
void turbo_destroy(turbo *t) {
    if (t->ram[1]) munmap(t->ram[1], 256u << 20);
    free(t->ram[4]); free(t->ram[5]); free(t->ram[6]); free(t->ram[12]); free(t->cache);
}
int turbo_load(turbo *t, const char *path, uint32_t a) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return -1; }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long n = ftell(f); rewind(f);
    uint8_t *p = n >= 0 ? turbo_ptr(t, a, (size_t)n) : NULL;
    int rc = !p || fread(p, 1, (size_t)n, f) != (size_t)n;
    fclose(f);
    if (rc) vv_eprintf("[turbo] invalid image range: %s@0x%08x\n", path, a);
    else vv_eprintf("[load] raw %s -> 0x%08x (%ld bytes)\n", path, a, n);
    return rc ? -1 : 0;
}
uint32_t turbo_sm_read(void *ctx, uint32_t a, unsigned n) { return turbo_load_mem(ctx, a, n, 0); }
