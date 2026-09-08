# Decisions

Dated, newest first. A decision stays until a later entry replaces it.

- **2026-09-09 · Scope of the first decoder.** Exactly the variants the two
  corpora contain: codec v1 and v2, lossless and three-level lossy, one or
  two tile columns, four planes, 14 bits, encoding type 0, no rounded bits,
  no per-subband partial q. Anything else returns `CRX_E_UNSUPPORTED` and
  OjoX keeps LibRaw for it. Open: encoding types 1 and 3, single plane,
  rounded bits, partial q, tile rows.
- **2026-09-09 · Corpus facts.** Kamen's archive holds no lossless CR3 at
  all (12,306 R6 Mark II files are v2 C-RAW with QP maps, 3,001 EOS R files
  are v1 C-RAW in two tiles); lossless coverage comes from the public
  samples. The 15,307 small preview tracks (1624x1080, C-RAW) are a free
  fast test set. Oracle fingerprints: all 15,307 files, 0 failures, in
  ~/libcrx-corpus/oracle-private.tsv (kept out of the repository; it is
  only useful to whoever has the files).
- **2026-09-09 · Seam extras are derived, not tabulated.** The number of
  extra coefficients a tile stores at a seam follows from a three-rule
  fixed point (SPEC 4.2), verified against the reference for every width
  22..3999 and every level count. The decoder computes it.
- **2026-09-09 · Integer overflow policy.** i32 with two's-complement wrap
  (`-fwrapv`); exactness is promised on conforming streams, memory safety
  on all input, no promise to match the reference's undefined behaviour.
- **2026-09-08 · Corpus.** Kamen's archive, deduplicated by SHA-256, is the
  private corpus: 15,307 unique CR3 files, 241 GB, EOS R and EOS R6 Mark II.
  Its manifest (hash, size, path) stays out of the repository; the oracle
  fingerprints carry no paths and may be published. The public corpus is
  every Canon CR3 sample on raw.pixls.us (110 files, CC0).
- **2026-09-08 · Oracle.** LibRaw 0.22.2 is the primary oracle. Its answer
  is recorded as a fingerprint (geometry, black levels, SHA-256 of the
  sensor buffer) so that later checks need neither LibRaw nor the time to run
  it, and so that a LibRaw upgrade cannot silently move the target. rawspeed
  is the second oracle, to be added when the first disagreement needs a
  tiebreak or at milestone 3, whichever is earlier.
- **2026-09-08 · Name and home.** `libcrx`, github.com/kamenlevi/libcrx,
  Apache-2.0, C11, a Swift wrapper later in OjoX.
- **2026-09-08 · Project approved** by Kamen: plan in `docs/PLAN.md`.
