# How libcrx decodes

A companion to `SPEC.md` (what the format is) and `docs/PLAN.md` (how the
project runs). This is how the code is arranged and why.

## Files

| file | job |
|---|---|
| `include/crx.h` | the public API: open, geometry per level, decode into a caller buffer, close |
| `src/container.c` | ISO BMFF walk to the image track; every read bounded |
| `src/headers.c` | CMP1, codestream headers, subband geometry with the seam-extras rule (SPEC 4.2) |
| `src/bits.h` | big-endian bit reader; reads past the end give zeros and are counted |
| `src/rice.h/.c` | Rice code with escape, sign mapping, adaptation, run mode (SPEC 5.2-5.5) |
| `src/lines.c` | the base-band and high-pass line decoders (SPEC 5.6, 5.7), into caller rows |
| `src/qp.c` | QP map decode and step tables (SPEC 7) |
| `src/wavelet.c` | 5/3 synthesis: one line; a stage split into a row pass and a column pass (SPEC 8) |
| `src/pool.c` | a small persistent pthread pool and a workspace cache |
| `src/decode.c` | the pipeline: lossless planes, and the phased parallel lossy pipeline |
| `tools/` | crxoracle (LibRaw fingerprints), crxcheck (exact comparison, headers-only, partial verification), crxbench |
| `tests/` | unit tests with an encoder written from the spec, the reference analysis, the partial-decode verifier |
| `fuzz/mutate.c` | mutation fuzzer for the sanitizer builds |

## The lossy pipeline

1. **Open** parses everything up to the first coded byte and computes every
   band's geometry. Nothing is decoded.
2. **Phase 1, bands.** Every (tile, plane, band) that the requested level
   needs is one task: decode its lines into a padded buffer (row -1 zero,
   one pad column each side, so the line decoders read neighbours without
   branches) and multiply each line by its quantisation step while it is
   still in cache. Bands are independent bitstreams, so with 40 or 80 of
   them the pool is busy; the wall time is the largest single band.
3. **Phase 2, synthesis in strips.** For each level from the coarsest
   down, the stage's output rows are cut into strips of 32; a strip task
   runs the horizontal synthesis for the few low and high rows it needs
   (recomputing one or two halo rows at its edges) into per-worker scratch,
   then the vertical lifting for its rows. Intermediate stages write the
   next level's low-pass band; the final stage hands each finished row to
   the emitter, which writes the mosaic with the median offset and the
   clamp. No full-plane temporaries exist: memory is the bands plus one
   intermediate band per plane.

A level-n decode stops phase 2 after level n+1 and emits the intermediate
band; phase 1 never touched the finer bands.

## Why these choices

- **Whole bands in memory, not a rolling window.** The reference streams a
  few rows at a time to save memory. We trade memory for independence: a
  band task owns its buffer, needs no synchronisation, and the synthesis
  passes become simple loops the compiler vectorises. The strips keep the
  synthesis itself in cache and free of temporaries.
- **Padded buffers.** The line decoders index `x - 1` and `x + 1` on the
  previous row; a pad column each side and a zero row above turn every
  boundary case into the general case, which is also how the format itself
  defines the first line.
- **Dequantisation fused per line.** A separate pass over 26 million
  coefficients cost 7 ms; the same multiply on a hot line costs almost
  nothing.
- **Shared pool and workspace cache.** A viewer decodes thousands of files
  in one process. Threads park between decodes; the large buffers are
  reused so pages fault once.
- **Diagnostics over debuggers.** `CRX_TRACE=1` prints phase times;
  `crx_overrun_bits` reports bits read past band data; the internal raw,
  extended and band decoders let the verifier see intermediates.

## Threads

`crx_decode(..., threads)` asks for that many workers including the
caller. The pool is process-wide and rebuilt only when a different count
is requested. Decoding two files at once from two threads is safe as long
as they use the same count (they share the pool); the decoder object
itself is not shared between threads.
