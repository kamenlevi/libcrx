# CRX: our own Canon RAW decoder

A plan for a decoder that reads Canon's CR3 sensor data faster than anything
that exists, proven exact, built like a product team would build it. Written
to be read cold. Nothing here is built yet.

## 1. Mission and the numbers that define "done"

Replace LibRaw's 43 ms unpack of a 24 MP CR3 with our own, and prove it right.

| criterion | target | how it is measured |
|---|---|---|
| exactness, full decode | every sensor value equal to LibRaw's, on 100% of the corpus | harness diff, bit for bit, integers |
| exactness, half size | equal to the low band of the exact integer wavelet of the full decode | harness diff against the reference transform |
| speed, half size, 24 MP C-RAW, M5 Pro | under 10 ms | bench, median of 24 files, cold cache |
| speed, full, 24 MP C-RAW | under 25 ms | same |
| robustness | 0 crashes, 0 hangs on fuzzed and truncated input | libFuzzer corpus, 24 h |
| memory | under 2x the output size in flight | measured per decode |
| portability | macOS arm64 and x86_64, Linux x86_64 and arm64 | CI on all four |
| licence | Apache-2.0 clean room, no LibRaw code | review of every file |

Out of scope: demosaic, colour and curve (OjoX has those), CR2 and other
makers, Canon's dual-pixel side data, HEIF-wrapped HIF files.

## 2. Roles

- **Kamen, product owner and quality.** Owns the corpus and the acceptance:
  runs the harness, reads its one screen, decides whether a milestone is
  green, names the file that broke. Owns the decision log.
- **Claude, engineering.** Design, the derived specification, every line of
  decoder code, every test, every benchmark, and the written derivation of
  every formula before it is coded.
- **Peter, integration and second reader.** Wires the library into OjoX
  behind LibRaw, carries it to Linux, reviews the specification for holes.

Cadence: milestones are gates, not dates. A gate opens when its acceptance
table is green on the full corpus and the bench has not regressed. Nothing
skips a gate.

## 3. The discipline that keeps the mathematics sound

The failure mode this project cannot afford is a decoder that is nearly
right. These rules are how "nearly" is made impossible:

1. **Two oracles, not one.** LibRaw's unpacked buffer is the primary
   reference. rawspeed's decoder is the second: when the two agree and we
   differ, we are wrong; when they disagree with each other, the file goes in
   the decision log and both outputs are kept.
2. **Integers only.** CRX is an integer format. The decoder uses no floating
   point anywhere; every intermediate has a written bit width and a proof it
   cannot overflow (the wavelet's growth per level is bounded and stated).
3. **Derivation before code.** Every formula, the Golomb-Rice parameter
   adaptation, the run-length rule, the lifting steps of the 5/3 wavelet, the
   quantisation, gets a page in `SPEC.md` with the formula, its source
   (Clévy's notes, LibRaw's behaviour observed on real files), a hand-worked
   example on a tiny vector, and the unit test that encodes that example.
   Code that has no derivation page does not merge.
4. **Exactness is a test, never a review.** The corpus gate runs on every
   commit: a fixed 200-file subset locally in under a minute, the full corpus
   nightly. A commit that changes any decoded value on any file is refused
   unless the decision log says why.
5. **Fuzzing from the first decoder line.** The entropy decoder is fuzzed the
   day it exists, with corrupted and truncated real files as seeds. A
   decoder that can be crashed by a file is not done.
6. **Performance is measured, never estimated.** A bench per commit, the
   summariser's one screen, a stored baseline; a regression over tolerance
   is a failed gate like any other.
7. **Clean room.** LibRaw is read to understand the format, then closed. Our
   code is written from `SPEC.md`. This keeps the licence clean and, more
   importantly, forces the understanding into words that can be checked.

## 4. Ground truth: the corpus and the harness

- **Corpus.** Kamen's archive (about 21,000 CR3 from the R6 Mark II, both
  RAW and C-RAW modes if present) plus the public sample set with a CR3 from
  every Canon body that writes one (raw.pixls.us, CC0), plus deliberately
  broken files. Every file listed with its hash; the list is the corpus.
- **Oracle dump.** A tool that writes, per file, LibRaw's unpacked sensor
  buffer and its metadata (dimensions, margins, bit depth, pattern, tiles) to
  a compact binary beside the corpus, once. rawspeed's output the same way.
- **Harness.** `crxcheck <files>`: decodes with ours, compares with the
  oracle, prints one line per file and one summary line. Exit code is the
  gate. `crxbench <files>`: times decodes, prints the summariser's format.
- **Reference transform.** A small, slow, obviously-correct implementation
  of the CRX integer wavelet analysis, used only to define what a half-size
  decode must equal. Written first, from the specification, and tested on
  hand-worked vectors.

## 5. Milestones

Estimates are for the engineering; the corpus gate decides when each is over.

| # | milestone | deliverable | gate |
|---|---|---|---|
| 0 | Foundations | repository, licence, CI on four platforms, corpus list with hashes, oracle dumps, harness and bench skeleton, `SPEC.md` outline | harness runs and reports 0 of N exact, honestly |
| 1 | Reading the format | `SPEC.md` sections for the container, the CRX headers, the tile and subband layout, the entropy coding, the wavelet, the quantisation; each with sources and worked examples | Peter's read: no unanswered question left in the text |
| 2 | Headers | our parser of every header field, compared with LibRaw's parse | 100% of the corpus |
| 3 | Lossless CRX | the entropy decoder; one plane exact, then all planes and tiles; fuzzing starts | 100% of lossless files exact; fuzzer clean for 24 h |
| 4 | Lossy C-RAW | subbands, dequantisation, the integer inverse wavelet | 100% of C-RAW files exact |
| 5 | Partial decode | stop after level N for half and quarter size; the API `decode(level:)` | equal to the reference transform on 100% |
| 6 | Speed | parallel across tiles, slices and subbands; a branchless table-driven Rice decoder; memory layout for the wavelet | half under 10 ms, full under 25 ms, no exactness change |
| 7 | Hardening and release | fuzz corpus grown, truncated and odd-size files, API frozen, documentation, tagged v1 | all gates green together, on all four platforms |
| 8 | Integration | OjoX uses it behind LibRaw as fallback, with a background self-check that develops one file in a hundred both ways and reports any difference | OjoX battery at parity or better; self-check silent for a week |

Engineering estimate, evenings and weekends: 0 and 1 in the first week, 2
and 3 in the following two, 4 in two more, 5 in one, 6 in two, 7 and 8 in
one. Six to nine weeks. The app loses nothing while it runs: LibRaw stays.

## 6. Design, as far as it can be said before milestone 1

- A C library with a small API: open from bytes, query the layout, decode a
  plane or the whole frame at a level into a caller-provided buffer, no
  allocation inside the hot path. A Swift wrapper for OjoX.
- Parallelism along the format's own seams: tiles, then the four colour
  planes, then subbands, each a stream that decodes independently. The
  wavelet reconstruction of a plane waits for its subbands and runs on the
  same core that decoded them.
- The half-size path decodes only the subbands the requested level needs and
  never touches the rest of the file, which is what makes it a quarter of
  the work, not a quarter of the output.
- Memory: coefficients for one plane at a time, reconstruction in place,
  output written straight into the caller's buffer.

## 7. Risks, named now

| risk | what we do about it |
|---|---|
| A body or mode the notes do not cover | the corpus has every body; a file that fails stays on LibRaw and goes in the log; the decoder reports "unsupported", never guesses |
| Ten milliseconds is not reached | the exact decoder still ships (correct at 43 ms is still a win on every other machine); speed work continues behind it |
| My own errors | derivation pages, two oracles, tests before code, fuzzing, and Kamen's harness run as the only definition of green |
| Time | small gates, each useful alone; LibRaw stays until milestone 8 |
| Linux and Intel | CI from milestone 0, not at the end |

## 8. Decision log

- 2026-09-08: project proposed, awaiting approval. Own repository, Apache-2.0, clean room,
  LibRaw and rawspeed as oracles, C with a Swift wrapper. Name to be chosen.
