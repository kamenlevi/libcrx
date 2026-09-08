# Decisions

Dated, newest first. A decision stays until a later entry replaces it.

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
