# libcrx

A decoder for Canon's CRX raw codec, the sensor data inside CR3 files.
Written from the format up, in C11, with no dependencies. Output is the
sensor values, exact; demosaic and colour are the caller's business.

Status: milestone 3. Headers of every corpus file parse; every lossless
public sample decodes bit-exact (38 files, 20 camera bodies). Lossy
C-RAW is milestone 4. See `docs/PLAN.md` for the milestones and `SPEC.md`
for the format as we derive it.

## Why

LibRaw unpacks a 24 megapixel C-RAW in about 43 ms on an M5 Pro. A viewer
that wants to show the real RAW while the user steps through a folder needs
that under 10 ms, and it needs a half-size decode that reads a quarter of
the file rather than a quarter of the output. The wavelet structure of C-RAW
allows both. Nobody had written the decoder that does it.

## Guarantees

- **Exact.** Every decoded value equals LibRaw's, bit for bit, on the whole
  corpus (about 15,000 private files plus every public sample), checked on
  every commit by `crxcheck`.
- **Integer.** No floating point anywhere. Every intermediate has a stated
  bit width.
- **Half size is defined, not approximated.** A level-N decode equals the LL
  band of the exact integer wavelet analysis of the full decode, N times.
- **Clean room.** Written from `SPEC.md`, which is derived from Laurent
  Clévy's CR3 documentation and from observing reference decoders as black
  boxes. Apache-2.0.

## Build

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build

`crxoracle`, the tool that records LibRaw's answers, needs LibRaw:
`-DCRX_LIBRAW_DIR=<prefix>` or `libraw_r` via pkg-config.

## Tools

- `crxoracle <files>` writes one fingerprint row per file: dimensions,
  margins, black levels, and the SHA-256 of LibRaw's sensor buffer. No paths.
- `crxcheck -o oracle.tsv <files>` decodes with libcrx and compares. One line
  per failure, one summary line, exit 0 only when everything is exact.
- `crxbench [-l level] [-t threads] <files>` times decodes from memory.

## API

See `include/crx.h`. Open, ask for the geometry at a level, decode into your
buffer, close. Nothing is allocated on the decode path.
