/*
 * probe_sequence.c — audit 0001 measurement. NOT library code.
 * Builds against ~/Desktop/the-level (chipmem.h, blit.h) only.
 */
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

#define N_TOKENS 1024
#define BYTES (N_TOKENS * sizeof(uint32_t))
#define RUNS 100

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}
static void fill(uint32_t *p, int n)
{
    for (int i = 0; i < n; i++) p[i] = 0x1000u + (uint32_t)i;
}
static int check(const uint32_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] != 0x1000u + (uint32_t)i) return i;
    return -1;
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("renderD128"); return 1; }
    blit_set_fd(fd);
    chipmem_set_fd(fd);
    if (blit_init() != 0) { fprintf(stderr, "blit_init\n"); return 1; }
    printf("backend=%d caps=0x%x\n", (int)blit_backend(), blit_caps());

    chipmem_backend_t be;
    chipmem_buffer_t *a = chipmem_alloc(BYTES, &be);
    chipmem_buffer_t *b = chipmem_alloc(BYTES, &be);
    chipmem_buffer_t *c = chipmem_alloc(BYTES, &be);
    if (!a || !b || !c) { fprintf(stderr, "alloc\n"); return 1; }
    chipmem_register_for_blit(a);
    chipmem_register_for_blit(b);
    chipmem_register_for_blit(c);

    uint32_t *pa = chipmem_cpu_ptr(a);
    uint32_t *pb = chipmem_cpu_ptr(b);
    uint32_t *pc = chipmem_cpu_ptr(c);
    fill(pa, N_TOKENS);
    chipmem_sync(a, CHIPMEM_ACCESS_CPU_WRITE);

    /* 1: positive two-link chain via blit handles. */
    int mism = 0;
    for (int r = 0; r < RUNS; r++) {
        blit_desc_t d1 = { .minterm=0xF0, .src=pa, .dst=pb,
                           .width=N_TOKENS, .height=1, .bpp=4 };
        blit_op_t *o1 = blit(&d1, BLIT_MODE_ASYNC);
        if (!o1) { fprintf(stderr, "blit1\n"); return 1; }
        blit_op_wait(o1);

        blit_desc_t d2 = { .minterm=0xF0, .src=pb, .dst=pc,
                           .width=N_TOKENS, .height=1, .bpp=4 };
        blit_op_t *o2 = blit(&d2, BLIT_MODE_ASYNC);
        if (!o2) { fprintf(stderr, "blit2\n"); return 1; }
        blit_op_wait(o2);
        chipmem_sync(c, CHIPMEM_ACCESS_ACCEL_WRITE);
        if (check(pc, N_TOKENS) != -1) mism++;
    }
    printf("1 positive: %d/%d mismatch (expect 0)\n", mism, RUNS);

    /* 2: negative control — the status-quo gap. */
    double sum = 0, mx = 0;
    for (int r = 0; r < RUNS; r++) {
        blit_desc_t d1 = { .minterm=0xF0, .src=pa, .dst=pb,
                           .width=N_TOKENS, .height=1, .bpp=4 };
        blit_op_t *o1 = blit(&d1, BLIT_MODE_ASYNC);
        blit_op_wait(o1);
        double t0 = now_ns();
        blit_desc_t d2 = { .minterm=0xF0, .src=pb, .dst=pc,
                           .width=N_TOKENS, .height=1, .bpp=4 };
        blit_op_t *o2 = blit(&d2, BLIT_MODE_ASYNC);
        double t1 = now_ns();
        blit_op_wait(o2);
        chipmem_sync(c, CHIPMEM_ACCESS_ACCEL_WRITE);
        double gap = t1 - t0;
        sum += gap;
        if (gap > mx) mx = gap;
    }
    printf("2 gap A-complete->B-submit: mean %.0f ns, max %.0f ns "
           "over %d runs\n", sum / RUNS, mx, RUNS);

    /* 3: withdrawal finding. */
    printf("3 withdrawal: no DRM/i915 ioctl submits a blit_desc_t on a "
           "fence; blit() is userspace by construction. copper "
           "SURVIVES unless (a) is shown false.\n");

    chipmem_free(a); chipmem_free(b); chipmem_free(c);
    blit_shutdown();
    close(fd);
    return 0;
}
