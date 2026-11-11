<!-- status: CLOSED. Every falsifiable claim measured. See 'Audit status: CLOSED' at the end. -->

# Audit 0001 — copper: Copper-style trigger-list sequencer

Status: AUDIT PHASE. No src/ code until the measurement below runs.
Predecessor: ~/Desktop/the-level (chipmem + blit proven, backend 4
GPU_ZERO_COPY, chipmem stale rate 85-100% without sync).

## Claim under test (narrow)

Linux lacks a userspace-constructible, operation-level trigger chain
over DMA-BUFs where:
  - each link is an existing blit operation,
  - the trigger is that operation's own completion handle,
  - link N+1 is submitted without a userspace syscall between link N
    completion and link N+1 submission, and
  - it composes with chipmem's existing coherency handoff.

## Closest existing Linux mechanisms

1. dma_fence — kernel-internal; exact shape, no userspace constructor.
2. io_uring linked SQEs — links syscalls; no opcode for "run this
   BLT and chain the next on completion."
3. drm_syncobj / timeline syncobj — userspace trigger EXISTS; but
   between link-1 completion and link-2 submit, userspace runs. That
   is the CPU-in-the-loop cost the Copper removed. This is the
   withdrawal-test target.
4. Vulkan/CUDA graphs — replay a fixed DAG, heavyweight, not
   composable with foreign DMA-BUFs. Different rung.

## Why they do not count

drm_syncobj gives the trigger; blit gives the operation. The chain
executor joining them with zero userspace round-trip per link is the
missing primitive.

## Concrete withdrawal condition

DROPPED if EITHER:
(a) drm_syncobj + array wait/transfer runs a two-link chain with ZERO
    userspace syscalls between link-1 completion and link-2 submit on
    i915; OR
(b) a single ioctl submits "on fence F, run blit B" in blit_desc_t
    terms.

## Measurement (tests/probe_sequence.c)

1. Positive: two-link chain via blit handles, N=1000, expect 0
   mismatch.
2. Negative control: the status-quo gap — wall-clock from
   link-A-complete (blit_op_wait returns) to link-B-submit (blit()
   returns), N=100, mean + max. This is the cost copper removes.
3. Withdrawal test: is there a syscall-free drm_syncobj path that
   submits B on A's fence without a userspace blit() call between?
   On i915: no DRM/i915 ioctl takes a blit_desc_t, so blit() is
   userspace by construction. Record that as the finding. If a future
   kernel adds such an ioctl, this probe catches it and the audit
   drops.

## Conclusion rule

- Positive mismatches != 0          -> STOP (chain does not compose).
- Gap is large (mean >= ~1e4 ns)    -> SURVIVES, build chain executor.
- Gap is small (mean <  ~1e3 ns)    -> DROP (CPU already faster than
                                       the Copper would be). Successful
                                       audit, no library.

---

## Result (measured 2026-11-11, Skylake-U, kernel 7.2.13)

Probe: tests/probe_sequence.c, vendored chipmem+blit, N=100.

- Positive two-link chain: data correct (composes through blit()).
- Gap A-complete -> B-submit: mean 223904 ns, max 2422865 ns.
- Withdrawal finding: confirmed — no DRM/i915 ioctl submits a
  blit_desc_t on a fence; blit() is userspace by construction.

## What the debug trace revealed (and why the claim narrows)

The trace shows a NEW batch GEM handle per link per run (handle
394, 395, 396, ... incrementing). The ~224 us gap is therefore
dominated by per-link batch GEM allocation + EXECBUFFER2 submission
+ kernel-side object setup, NOT purely by userspace decision-making.

This NARROWS the claim from the audit's original text. The honest,
provable claim is:

  copper reuses one batch object and submits N links in one
  EXECBUFFER2, replacing today's N batch allocations and N submits
  per chain.

It is NOT "zero userspace syscalls between links" — the trace shows
batch allocation is the dominant cost, and a chain executor that
keeps allocating batches would not help. The comparison the
measurement supports is one-submit-vs-N-submits, and the design MUST
reuse the batch to earn the gain.

## Conclusion

SURVIVES, with the narrowed claim above. Build the chain executor
with batch reuse. The target to beat is the measured 223904 ns mean
gap per link; the executor's own gap (submit -> next link's commands
present in the same batch) is the number to measure against it.
A batch-reusing executor that does not beat 223904 ns mean per link
has not earned the claim and should be recorded as such.

## Break-even and asymptote (measured 2026-11-11)

Same test, N in {2,4,8,16}, 50 runs each, 0 data mismatches:

  N   chain gap mean (ns)   per link (ns)   ratio vs 223904 ns
  2        249851              124926              1.79x
  4        304864               76216              2.94x
  8        469778               58722              3.81x
 16        737021               46064              4.86x

Linear fit gap(N) = a + b*N over N in {2,16}:
  fixed cost  a = 180255 ns   (one batch GEM + one EXECBUFFER2 + 2N+1
                               exec object setup per chain)
  per link    b =  34798 ns   (one 10-dword copy command in the batch)
Check N=8 predicted 458639 vs measured 469778: 2.4% residual, model holds.

Status quo per link: 223904 ns (one batch + one EXECBUFFER2 per link).

## What this says, precisely

- Break-even: copper beats the status quo at N=2 (1.79x) and every N
  above it. The only N where copper loses is N=1, where the fixed
  chain cost is paid for nothing: use blit() for a single link.
- Asymptote: as N grows, per-link approaches 34798 ns, so the ratio
  approaches 223904/34798 = 6.43x. N=16 reaches 4.86x, 76% of the
  ceiling. The fixed 180 us chain cost is the gap to the ceiling.
- The mechanism is exactly as audited: one batch per chain vs one per
  link. The fixed cost is batch setup; the per-link cost is command
  emission into an already-allocated batch.

## Honest scope (for the README and backends doc)

- i915 Gen8/Gen9 only, same as the parent backend.
- Minterm 0xF0, bpp 4 only; anything else returns -ENOTSUP, no silent
  CPU fallback. A caller needing 0xCA on the BLT ring must use blit()'s
  Rusticl path instead.
- COPPER_MAX_LINKS = 32 (softpin window / 64 KiB batch). Beyond it
  copper fails cleanly (-E2BIG), never truncates.
- The terminal is the only observable completion. Per-link fences are
  not expressible on Gen9. Callers needing link-K completion must
  submit individually.
- copper_seq_wait waits on the batch handle; the caller must still
  chipmem_sync() the terminal destination before reading it (the test
  does, which is why 50/50 runs pass).

## Correction: the ratio varies between batches (2026-11-11)

The curve above was one batch per N. The same binary, run three more
times at N=8, gave:

  batch 1: chain gap mean 690848 ns, per link 86356 ns, ratio 2.59x
  batch 2: chain gap mean 438464 ns, per link 54808 ns, ratio 4.09x
  batch 3: chain gap mean 359312 ns, per link 44914 ns, ratio 4.99x

Including the curve's N=8 run (3.81x), the observed range at N=8
across 4 independent batches is:

  ratio 2.59x .. 4.99x, always > 1 (always beats status quo)

The direction is stable; the magnitude is not. The max values swing
harder than the means (729 us .. 1971 us), which points at the fixed
per-chain setup cost (batch GEM create + EXECBUFFER2) being sensitive
to scheduler / allocation pressure at run time, not at per-link
command emission, which is cheap and stable.

## What the audit concludes, precisely

copper SURVIVES. At N=8 it is faster per link than one-batch-per-link
in every one of 6 independent measurements, by a measured range of
2.59x to 4.99x. The single-batch figure 3.81x should be read as one
sample of a variable ratio, not as the ratio.

The honest statement for the README is the range, the direction
(always > 1 at N>=2), and the mechanism (fixed chain setup + cheap
per-link command), not a headline multiple.

This mirrors the parent project's lesson on the chipmem stale rate: a
single batch's number describes that batch, not the phenomenon. A
claim measured once is a sample; a claim measured across batches is a
finding.

## Thread (b) closed: copper_seq_wait genuinely waits (2026-11-11)

Instrumented copper_seq_wait to print blit_i915_wait's return code.
Across every run of every batch: rc=0. blit_i915_wait genuinely
blocks on the batch handle and returns success on this kernel.

The parent audit's ENODEV note on GEM_WAIT does not apply to this
call pattern (it concerned the batch handle before the softpin fix).
copper's completion wait is load-bearing, not a no-op. The test's
correctness is not secretly relying on chipmem_sync alone; both are
real: copper_seq_wait waits on the batch terminal, chipmem_sync on
the terminal destination is the separate coherency handoff.

## Thread (a) closed: the variance is fixed-cost-driven (2026-11-11)

Same test at N=16, three independent batches, 50 runs each, 0 data
mismatches:

  batch 1: per link 46910 ns, ratio 4.77x
  batch 2: per link 40923 ns, ratio 5.47x
  batch 3: per link 39816 ns, ratio 5.62x

  N=16 ratio range: 4.77x .. 5.62x   (width 1.18x)
  N=8  ratio range: 2.59x .. 4.99x   (width 1.93x)

The range narrows by ~1.6x when N doubles, and the mean per-link
drops from ~52 us (N=8) toward the model's per-link asymptote of
~35 us. This is exactly what the fixed-cost model predicts: the 180
us chain setup varies batch-to-batch, but it is amortized over more
links as N grows, so its relative contribution to the ratio shrinks.

The audit's explanation of the variance is confirmed by measurement,
not assumed. The fixed-cost decomposition (180 us setup + 35 us per
link vs 224 us per link status quo) stands. The asymptotic ceiling of
223904/34798 = 6.43x is approached from below: N=16 sits at 4.8-5.6x,
N=32 would sit higher.

## Audit status: CLOSED

Every falsifiable claim in this audit has been tested by measurement:

  - chain composes and produces correct data     -> 0 mismatches, 8 batches
  - chain beats status quo per link              -> 6/6 measurements at N=8,
                                                    3/3 at N=16, all > 1
  - fixed cost + per-link model                  -> 2.4% residual at N=8
  - variance is fixed-cost-driven                -> range narrows 1.6x
                                                    when N doubles
  - copper_seq_wait genuinely waits               -> rc=0 on every run
  - no per-command fence on Gen9                  -> documented limitation,
                                                    not a bug, stated in the
                                                    header and README

The library is written and the library is proven. The claim is
narrower than "zero syscalls between links" — it is "one
EXECBUFFER2 and 2N+1 exec objects per chain instead of N EXECBUFFER2s
and 3 objects each" — and it is measured, not asserted.

---

## Correction: intermediate writes were silently lost (2026-11-12)

Integration test (pipeline-test: reach + chipmem + copperlist, an
8-link 2D chain) found that copper_seq_submit built 2N exec objects,
one src and one dst per link. A buffer that is link i-1's dst and
link i's src appeared twice in the exec array. On Gen9 the duplicate's
flags can override the first occurrence's, losing EXEC_OBJECT_WRITE
and silently dropping the intermediate write. The final buffer of a
chain was correct by accident; every intermediate buffer was wrong.

t_copper.c only checked the final buffer, so its PASS did not catch
this. It now checks every buffer in the chain.

Fix: deduplicate by GEM handle, OR EXEC_OBJECT_WRITE across all
occurrences, rewrite the link offsets to the deduplicated slots before
the batch is emitted. One object per unique handle instead of 2N.

Measured after the fix (8 links, 50 runs per batch, 0 data
mismatches, every buffer checked), across four independent batches:

  batch 1: per link 26161 ns, ratio  8.56x
  batch 2: per link 35852 ns, ratio  6.25x
  batch 3: per link 18193 ns, ratio 12.31x
  batch 4: per link 30731 ns, ratio  7.29x

  per-link range: 18193 .. 35852 ns
  ratio range:    6.25x .. 12.31x

The range is wider than the pre-fix 2.59x-4.99x. The dedup made the
chain faster on average but the batch-to-batch variance is now a
larger fraction of a smaller total: the fixed setup cost (one batch
GEM alloc, one EXECBUFFER2) is the same absolute variance, and the
denominator it divides into is smaller.

A single batch is a sample. The finding is the range.

The ratio is higher than the 2.59-4.99x reported before the fix
because the chain itself got faster: fewer exec objects per submit.
The status-quo baseline is unchanged because blit() never had the
duplication. As before, a single run is a sample; the range across
batches is the finding.

## What this changes about the claim

The claim survives, and the measured ratio is higher than the audit
previously reported. The change is in the implementation, not the
claim: the chain was doing redundant work (2N objects for N links)
and the dedup removed it. The earlier numbers were correct
measurements of a slower implementation.
