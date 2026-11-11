#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "chipmem.h"
#include "blit.h"
#include "copper.h"

extern int copper_register_ptr(const void *p, uint32_t h);

#define TOKENS 1024
#define BYTES (TOKENS * sizeof(uint32_t))
#define LINKS 8
#define RUNS  50
#define STATUS_QUO_GAP_NS 223904.0

static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("renderD128"); return 1; }
    blit_set_fd(fd); chipmem_set_fd(fd);
    if (blit_init() != 0) { fprintf(stderr, "blit_init\n"); return 1; }

    chipmem_backend_t be;
    chipmem_buffer_t *bufs[LINKS + 1];
    uint32_t *ptrs[LINKS + 1];
    for (int i = 0; i <= LINKS; i++) {
        bufs[i] = chipmem_alloc(BYTES, &be);
        if (!bufs[i]) return 1;
        chipmem_register_for_blit(bufs[i]);
        ptrs[i] = chipmem_cpu_ptr(bufs[i]);
        uint32_t h;
        if (chipmem_gem_handle(bufs[i], &h) != 0) { fprintf(stderr, "gem_handle\n"); return 1; }
        copper_register_ptr(ptrs[i], h);
    }
    for (int i = 0; i < TOKENS; i++) ptrs[0][i] = 0x1000u + i;
    chipmem_sync(bufs[0], CHIPMEM_ACCESS_CPU_WRITE);

    double sum = 0, mx = 0;
    for (int r = 0; r < RUNS; r++) {
        for (int i = 1; i <= LINKS; i++) memset(ptrs[i], 0, BYTES);
        double t0 = now_ns();
        copper_seq_t *s = copper_seq_create();
        for (int i = 0; i < LINKS; i++)
            if (copper_seq_add(s, ptrs[i], ptrs[i+1], TOKENS, 1) != 0) { fprintf(stderr, "add\n"); return 1; }
        copper_seq_op_t *op = copper_seq_submit(s, fd);
        if (!op) { fprintf(stderr, "submit: %s\n", strerror(errno)); return 1; }
        copper_seq_wait(op);
        double t1 = now_ns();
        chipmem_sync(bufs[LINKS], CHIPMEM_ACCESS_ACCEL_WRITE);
        copper_seq_destroy(s);

        /* Check every buffer, not just the final one. A chain can
         * produce a correct final buffer while an intermediate write
         * is silently lost. */
        for (int b = 1; b <= LINKS; b++) {
            int bad = 0;
            for (int i = 0; i < TOKENS; i++)
                if (ptrs[b][i] != (uint32_t)(0x1000u + i)) { bad = i; break; }
            if (bad) {
                fprintf(stderr, "run %d: buf[%d] mismatch at %d\n",
                        r, b, bad);
                return 1;
            }
        }

        double gap = t1 - t0;
        sum += gap; if (gap > mx) mx = gap;
    }
    double mean = sum / RUNS;
    double per_link = mean / LINKS;
    printf("chain: %d links, %d runs\n", LINKS, RUNS);
    printf("chain gap: mean %.0f ns, max %.0f ns\n", mean, mx);
    printf("per link:  mean %.0f ns\n", per_link);
    printf("status quo per link: %.0f ns (audit 0001)\n", STATUS_QUO_GAP_NS);
    printf("ratio: %.2fx faster per link\n", STATUS_QUO_GAP_NS / per_link);
    if (per_link < STATUS_QUO_GAP_NS)
        printf("PASS: chain beats status-quo gap per link\n");
    else
        printf("FAIL: chain does not beat status quo; audit must record\n");

    for (int i = 0; i <= LINKS; i++) chipmem_free(bufs[i]);
    blit_shutdown(); close(fd);
    return per_link < STATUS_QUO_GAP_NS ? 0 : 1;
}
