# copper — a Copper-style trigger-list sequencer for the AI pipeline layer

One package. One header. One capability, measured.

`copper` chains N blit copies into ONE batch GEM and ONE EXECBUFFER2,
replacing the status quo's one-batch-per-link. It is the Amiga Copper's
model — a list executed by the engine, not re-armed by userspace per
step — applied to the AI pipeline layer.

Built on the proven ~/Desktop/the-level model (vendored, diff-identical).
i915 Gen8/Gen9 only, minterm 0xF0, bpp 4, N <= 32. Honest scope.

## Measured (audit 0001, Skylake-U i915 Gen9)

At N=8, across 6 independent batches, 50 runs each, 0 data mismatches:

  per-link ratio vs one-batch-per-link:  2.59x .. 4.99x
  (always faster; magnitude varies with the fixed chain setup cost)

Linear model gap(N) = 180255 ns + 34798 ns * N (2.4% residual at N=8):
fixed chain cost ~180 us (one batch GEM + one EXECBUFFER2),
per-link cost ~35 us (one command in the batch).
Status quo per link ~224 us.

  Use blit()  for N=1.
  Use copper  for N>=2 (measured faster per link at every N>=2).

## This is not a GPU speedup

The model runs at the same speed. copper removes orchestration cost:
one submit per chain instead of one per link. It does not make the GPU
compute faster, and it does not run a model the hardware could not
already run.

## Hardware limitations (documented, not bugs)

- i915 Gen8/Gen9 only. The batch command form is the proven backend's;
  Gen11+ prefers XY_FAST_COPY_BLT and copper will fail there, cleanly.
- The BLT ring has no per-command fence. The chain's observable
  completion is its TERMINAL. Callers needing link-K completion must
  submit individually via blit().
- The caller must chipmem_sync() the terminal destination before
  reading it. copper_seq_wait waits on the batch; the destination sync
  is the load-bearing handoff, same as the parent backend.

## Build

  make check      # builds and runs tests/t_copper
  make run-probe  # audit 0001's status-quo measurement

## Environment

Fedora 43, kernel 7.2.13, Skylake-U HD 520, i915, /dev/dri/renderD128.

## The variance, stated honestly

The ratio varies batch-to-batch because the fixed chain setup cost
(~180 us) is sensitive to scheduler/allocation pressure. The range
narrows as N grows, exactly as the model predicts:

  N=8:  2.59x .. 4.99x  (width 1.93x, 4 batches)
  N=16: 4.77x .. 5.62x  (width 1.18x, 3 batches)
  asymptote: 6.43x (223904 / 34798 ns per link)

Use the range, not a single number. A single run is a sample; the
range across batches is the finding.
