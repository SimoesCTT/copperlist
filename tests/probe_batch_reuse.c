/*
 * probe_batch_reuse.c — audit 0001, second measurement.
 *
 * The first probe measured the status quo (new batch per link). This
 * one measures the claim: N links in ONE batch object, ONE submit.
 *
 * We do NOT write copper library code yet. We hand-build the batch
 * command stream using the vendored blit_i915 internal path if it is
 * reachable, or — if it is not — we record that the honest comparison
 * cannot be measured through the public blit() API, which is itself
 * an audit finding (the public contract cannot express batch reuse,
 * which is precisely why copper is needed).
 */
#include <stdio.h>
#include <time.h>
static double now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
int main(void){
    /*
     * Honest result of this probe, given the parent API: blit() does
     * not expose batch reuse. There is no public way to submit N
     * links in one EXECBUFFER2 through blit.h. That absence IS the
     * primitive copper restores.
     *
     * Record that. Do not fake a measurement the public API cannot
     * produce. The next step is to implement the batch-reuse path in
     * copper and measure it against 223904 ns.
     */
    printf("probe_batch_reuse: public blit() cannot express N links in "
           "one batch; that absence is the primitive. copper will "
           "implement it and measure against the 223904 ns mean gap.\n");
    return 0;
}
