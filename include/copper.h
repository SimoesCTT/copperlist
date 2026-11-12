#ifndef COPPER_H
#define COPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * copper — a Copper-style trigger-list sequencer for the AI pipeline
 * layer.
 *
 * Status quo (measured, audit 0001, Skylake-U i915 Gen9): one batch
 * GEM + one EXECBUFFER2 per blit link, ~224 us mean gap between
 * link-A completion and link-B submit. Dominated by per-link batch
 * allocation + submission, not by syscall count.
 *
 * copper's claim: N links in ONE batch GEM, ONE EXECBUFFER2. The
 * batch reuse and the per-link softpin slot reservation are the
 * primitive; the BLT ring already executes a batch sequentially.
 *
 * HARDWARE LIMITATION, documented, not a bug: on i915 Gen9 the BLT
 * ring gives no per-command fence. The chain's observable completion
 * is the chain's TERMINAL. A caller that needs link-K (K<N)
 * completion cannot learn it from copper. If you need that, submit
 * links individually via blit().
 *
 * Also: minterm 0xF0 (copy), bpp 4 only. This is what the proven
 * backend expresses; anything else returns -ENOTSUP. Honest scope,
 * not a hidden fallback.
 */

typedef struct copper_seq    copper_seq_t;
typedef struct copper_seq_op copper_seq_op_t;

/* Chain construction. */
copper_seq_t *copper_seq_create(void);

/*
 * Append one copy link (minterm 0xF0, bpp 4). src and dst are the CPU
 * pointers from chipmem_cpu_ptr(); both buffers must already be
 * chipmem_register_for_blit()'d with the same render node copper is
 * submitting on.
 * Returns 0 on success, negative errno on failure.
 */
int copper_seq_add(copper_seq_t *seq, const void *src, void *dst,
                   uint32_t width, uint32_t height);

/*
 * Submit the chain. Allocates ONE batch GEM, reserves one softpin
 * slot per src and per dst, writes N copy commands, submits ONE
 * EXECBUFFER2. Returns the terminal handle.
 * Requires drm_fd == the fd passed to blit_set_fd()/chipmem_set_fd()
 * (same GEM namespace).
 * Returns NULL and sets errno on failure. Chain remains owned by caller.
 */
copper_seq_op_t *copper_seq_submit(copper_seq_t *seq, int drm_fd);

/*
 * Wait for the chain terminal. This issues blit_i915_wait() on the
 * batch, which the parent audit records as the load-bearing wait on
 * this kernel (GEM_WAIT on the batch handle). Returns 0 on success,
 * negative errno on failure. Handle is consumed.
 */
int copper_seq_wait(copper_seq_op_t *op);

/*
 * Non-blocking: 1 if the batch has been accepted and drained, 0 if
 * still in flight, negative errno on failure. On Gen9 this is a
 * best-effort check (no per-command fence); documented as such.
 */
int copper_seq_done(copper_seq_op_t *op);

/* Release terminal handle without waiting. */
void copper_seq_op_release(copper_seq_op_t *op);

/* Free chain. Null-safe. */
void copper_seq_destroy(copper_seq_t *seq);

/*
 * LIMITATION (v0.1.5): copper_seq_add assigns each link a distinct
 * 2 MiB slot (COPPER_SRC_BASE + n * COPPER_SLOT_STRIDE, and the same
 * for dst), where n is the link index. It does not derive the offset
 * from the pointer's position within its buffer. Consequence: a chain
 * works when each link targets a distinct buffer, and does not work
 * when many links target different regions of the SAME buffer — the
 * second and later links address 2 MiB past the buffer and their
 * writes are lost. Found by copper-train, which chains 120 regions of
 * one 4 MB tensor and observes region 0 correct and every later
 * region zero at every link count from 2 to 32.
 *
 * A fix requires the API to take (buffer, offset) rather than a raw
 * pointer, so the offset within the buffer is known. That is a v0.2.0
 * change and is not made here.
 */
/* Number of links queued. */
size_t copper_seq_length(const copper_seq_t *seq);

/* Maximum links per chain (softpin window / batch-size bound). */
#define COPPER_MAX_LINKS 32

#ifdef __cplusplus
}
#endif
#endif /* COPPER_H */
