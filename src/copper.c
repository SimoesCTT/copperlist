/*
 * copper.c — chain executor over the i915 BLT ring.
 *
 * The vendored blit_i915.c pins every link's src/dst at the SAME
 * softpin VA (SRC_ADDR / DST_ADDR), which is why the status quo
 * allocates one batch per link. copper owns its own address layout:
 * one batch GEM, N distinct src slots, N distinct dst slots, one
 * EXECBUFFER2 with 2N+1 objects. This is what the audit measures.
 *
 * Command encoding copied exactly from the proven backend
 * (blit_i915_copy, vendored): header (2<<29)|(0x53<<22)|(1<<21)|(1<<20)|8,
 * followed by pitch/rop, base lo/hi, size, src pitch, src base lo/hi.
 * One MI_BATCH_BUFFER_END (0x05000000) terminates the batch.
 */
#define _GNU_SOURCE
#include "copper.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/i915_drm.h>
#include <drm/drm.h>

/* Exported by the vendored blit_i915.c. */
int  blit_i915_wait(uint32_t batch_handle, int64_t timeout_ns);
/* Pointer -> GEM handle registry (exported). */
int  blit_i915_register_ptr(const void *ptr, uint32_t gem_handle, uint32_t pitch);
int  blit_i915_unregister_ptr(const void *ptr);
/* find_reg is static; copper uses chipmem_gem_handle instead, which is
 * the public symbol and is what the registry was populated from. */
#include "chipmem.h"
int chipmem_gem_handle(chipmem_buffer_t *buf, uint32_t *out_handle);
/* copper needs ptr->buffer. The public API exposes only gem_handle
 * per buffer, not per ptr. Use the registry the vendored backend
 * maintains: find via a exported shim we add below. */

/* Address layout for a chain. Generous 2 MiB stride, 64 slots each. */
#define COPPER_SRC_BASE   0x01000000ULL   /* 16 MiB */
#define COPPER_DST_BASE   0x10000000ULL   /* 256 MiB */
#define COPPER_BATCH_ADDR 0x40000000ULL   /* 1 GiB */
#define COPPER_SLOT_STRIDE 0x00200000ULL  /* 2 MiB */
#define COPPER_BATCH_BYTES 0x00010000     /* 64 KiB */

struct link {
    uint32_t src_handle, dst_handle;
    uint64_t src_off,  dst_off;
    uint32_t width, height;
};

struct copper_seq {
    struct link *links;
    size_t       n, cap;
};

struct copper_seq_op {
    uint32_t batch_handle;
    int      drm_fd;
};

/* ---- registry shim: pointer -> GEM handle ---------------------------
 * The vendored blit_i915.c keeps a static registry (g_reg) populated
 * by chipmem_register_for_blit(). It exports blit_i915_register_ptr
 * and blit_i915_unregister_ptr but not the lookup. The proven
 * blit_i915_copy_from_ptr calls the static find_reg. copper cannot
 * call find_reg, so it must either (a) be given chipmem_buffer_t*
 * directly, or (b) reuse the pointer and let blit_i915_copy_from_ptr
 * do the lookup. Option (b) is cleaner: copper passes the CPU
 * pointers to blit_i915_copy_from_ptr with out_*_handle to learn the
 * GEM handles, then uses those for its own multi-slot layout. */

int blit_i915_copy_from_ptr(const void *src, void *dst,
                            uint32_t width, uint32_t height, uint8_t bpp,
                            uint32_t src_stride, uint32_t dst_stride,
                            uint32_t *out_src_handle,
                            uint32_t *out_dst_handle,
                            uint32_t *out_batch);

/* ---- chain ---------------------------------------------------------- */

copper_seq_t *copper_seq_create(void) { return calloc(1, sizeof(copper_seq_t)); }
size_t copper_seq_length(const copper_seq_t *s) { return s ? s->n : 0; }

void copper_seq_destroy(copper_seq_t *s)
{
    if (!s) return;
    free(s->links);
    free(s);
}

int copper_seq_add(copper_seq_t *seq, const void *src, void *dst,
                   uint32_t width, uint32_t height)
{
    if (!seq || !src || !dst || !width || !height) return -EINVAL;
    if (seq->n >= COPPER_MAX_LINKS) return -E2BIG;

    /* Ask the proven backend to resolve pointer -> GEM handle. We pass
     * width=height=1 so no real work is done on the ring; only the
     * registry lookup matters. But blit_i915_copy_from_ptr SUBMITS a
     * batch, which we do not want here. So instead we resolve via
     * chipmem_gem_handle, which needs the chipmem_buffer_t*, not the
     * raw pointer. copper's API takes raw pointers (like blit.h), so
     * we reach the registry through the exported register path in
     * reverse: the registry is keyed by pointer, and blit_i915 has no
     * exported lookup.
     *
     * Resolution: use the pointer as the key by calling the tiny
     * exported probe find_reg-equivalent. Since the vendored file does
     * not export find_reg, copper keeps its own pointer->handle map,
     * populated by copper_seq_add's caller via copper_register_ptr()
     * (added below). This is the honest seam: copper owns the map it
     * needs. */

    uint32_t sh = 0, dh = 0;
    extern int copper_lookup_ptr(const void *p, uint32_t *out);
    if (copper_lookup_ptr(src, &sh) != 0) return -ENOENT;
    if (copper_lookup_ptr(dst, &dh) != 0) return -ENOENT;

    if (seq->n == seq->cap) {
        size_t nc = seq->cap ? seq->cap * 2 : 8;
        struct link *nl = realloc(seq->links, nc * sizeof(*nl));
        if (!nl) return -ENOMEM;
        seq->links = nl; seq->cap = nc;
    }
    struct link *l = &seq->links[seq->n];
    l->src_handle = sh;
    l->dst_handle = dh;
    l->src_off = COPPER_SRC_BASE + seq->n * COPPER_SLOT_STRIDE;
    l->dst_off = COPPER_DST_BASE + seq->n * COPPER_SLOT_STRIDE;
    l->width = width; l->height = height;
    seq->n++;
    return 0;
}

/* ---- pointer -> handle map owned by copper ------------------------- */

struct pmap { const void *p; uint32_t h; };
static struct pmap g_pmap[256];
static size_t g_pmap_n;

int copper_register_ptr(const void *p, uint32_t h)
{
    if (!p || g_pmap_n >= 256) return -ENOSPC;
    g_pmap[g_pmap_n++] = (struct pmap){ .p = p, .h = h };
    return 0;
}
int copper_lookup_ptr(const void *p, uint32_t *out)
{
    for (size_t i = 0; i < g_pmap_n; i++)
        if (g_pmap[i].p == p) { *out = g_pmap[i].h; return 0; }
    return -ENOENT;
}

/* ---- command emission (encoding copied from vendored backend) ------ */

static uint32_t *emit_link(uint32_t *p, const struct link *l)
{
    uint32_t dst_pitch = l->width * 4;
    uint32_t src_pitch = l->width * 4;
    uint32_t cmd = ((2u << 29) | (0x53u << 22));
    cmd |= (1u << 21) | (1u << 20) | 8u;

    *p++ = cmd;
    *p++ = (3u << 24) | (0xccu << 16) | dst_pitch;  /* rop F0, dst pitch */
    *p++ = 0u;
    *p++ = (l->height << 16) | (l->width & 0xFFFF);
    *p++ = (uint32_t)(l->dst_off & 0xFFFFFFFF);
    *p++ = (uint32_t)(l->dst_off >> 32);
    *p++ = 0u;
    *p++ = src_pitch;
    *p++ = (uint32_t)(l->src_off & 0xFFFFFFFF);
    *p++ = (uint32_t)(l->src_off >> 32);
    /* no per-link terminator; one at the end of the batch */
    return p;
}

/* ---- submit -------------------------------------------------------- */

copper_seq_op_t *copper_seq_submit(copper_seq_t *seq, int drm_fd)
{
    if (!seq || seq->n == 0 || drm_fd < 0) { errno = EINVAL; return NULL; }
    size_t N = seq->n;

    /* One batch GEM for the whole chain. */
    struct drm_i915_gem_create create = { .size = COPPER_BATCH_BYTES, 0 };
    if (ioctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) return NULL;

    struct drm_i915_gem_mmap_offset mo = {
        .handle = create.handle,
        .flags  = I915_MMAP_OFFSET_WC,
    };
    if (ioctl(drm_fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo) != 0)
        goto err_batch;
    void *map = mmap(NULL, COPPER_BATCH_BYTES, PROT_READ | PROT_WRITE,
                     MAP_SHARED, drm_fd, (off_t)mo.offset);
    if (map == MAP_FAILED) goto err_batch;

    uint32_t *p = map;
    for (size_t i = 0; i < N; i++) p = emit_link(p, &seq->links[i]);
    *p++ = 0x05000000u;
    *p++ = 0x00000000u;
    uint32_t batch_len = (uint32_t)((uint8_t *)p - (uint8_t *)map);
    munmap(map, COPPER_BATCH_BYTES);

    /* 2N + 1 exec objects. Every distinct handle gets its own slot;
     * a src and dst that are the same GEM handle still need separate
     * objects (different offsets). Deduplicate handles would fail the
     * BLT ring's read/write domain — do not. */
    size_t nobj = 2 * N + 1;
    struct drm_i915_gem_exec_object2 *objs = calloc(nobj, sizeof(*objs));
    if (!objs) goto err_batch;
    for (size_t i = 0; i < N; i++) {
        objs[2*i].handle   = seq->links[i].src_handle;
        objs[2*i].offset   = seq->links[i].src_off;
        objs[2*i].flags    = EXEC_OBJECT_PINNED |
                             EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
        objs[2*i+1].handle = seq->links[i].dst_handle;
        objs[2*i+1].offset = seq->links[i].dst_off;
        objs[2*i+1].flags  = EXEC_OBJECT_PINNED |
                             EXEC_OBJECT_SUPPORTS_48B_ADDRESS |
                             EXEC_OBJECT_WRITE;
    }
    objs[2*N].handle = create.handle;
    objs[2*N].offset = COPPER_BATCH_ADDR;
    objs[2*N].flags  = EXEC_OBJECT_PINNED |
                       EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

    struct drm_i915_gem_execbuffer2 exec;
    memset(&exec, 0, sizeof(exec));
    exec.buffers_ptr        = (uint64_t)(uintptr_t)objs;
    exec.buffer_count       = (uint32_t)nobj;
    exec.batch_start_offset = 0;
    exec.batch_len          = batch_len;
    exec.flags              = I915_EXEC_BLT | I915_EXEC_NO_RELOC;

    int rc = ioctl(drm_fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec);
    free(objs);
    if (rc != 0) goto err_batch;

    copper_seq_op_t *op = calloc(1, sizeof(*op));
    if (!op) goto err_batch;
    op->batch_handle = create.handle;
    op->drm_fd = drm_fd;
    return op;

err_batch:
    {
        struct drm_gem_close c = { .handle = create.handle };
        ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &c);
    }
    errno = EIO;
    return NULL;
}

int copper_seq_wait(copper_seq_op_t *op)
{
    if (!op) return -EINVAL;
    int rc = blit_i915_wait(op->batch_handle, INT64_MAX);
    struct drm_gem_close c = { .handle = op->batch_handle };
    ioctl(op->drm_fd, DRM_IOCTL_GEM_CLOSE, &c);
    free(op);
    return rc;
}
int copper_seq_done(copper_seq_op_t *op) { return op ? 1 : -EINVAL; }
void copper_seq_op_release(copper_seq_op_t *op)
{
    if (!op) return;
    struct drm_gem_close c = { .handle = op->batch_handle };
    ioctl(op->drm_fd, DRM_IOCTL_GEM_CLOSE, &c);
    free(op);
}
