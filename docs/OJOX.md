# Integration into OjoX (milestone 8)

What the viewer does today: LibRaw opens the CR3 (metadata), unpacks the
sensor data (43 ms on 4 threads for a 24 MP C-RAW), and OjoX's own develop
stage (RawDevelop.swift: black, white balance, camera matrix, curve) makes
the picture. The develop stage was written against LibRaw's `raw_image`
mosaic and its metadata (black levels, `cam_mul`, `rgb_cam`).

## Step 1: swap the unpack, keep the metadata

- Build libcrx as a static library with CMake into `Vendor/libcrx/{include,lib}`
  (arm64 first; Intel and Linux from the same CMake).
- SwiftPM: a `systemLibrary` target `CCRX` wrapping `crx.h`, like `CLibRaw`.
- In `RawDeveloper.developOnPool`: after `libraw_open_buffer` (metadata
  only, no `libraw_unpack`), call `crx_open` on the same bytes and
  `crx_decode` at the level the request needs (see below) into a `uint16`
  buffer, then hand that buffer plus LibRaw's metadata to
  `RawDevelop.halfSize/fullSize`, which today read `raw_image`. The
  geometry to pass along: `crx_info.width/height` equal LibRaw's
  `raw_width/raw_height`; margins keep coming from LibRaw's `sizes`.
- Fallback: any status other than `CRX_OK` (unsupported variant, corrupt
  file) falls back to `libraw_unpack` exactly as now. Nothing is lost.
- Threads: `crx_decode(..., threads)` with the pool's worker count; libcrx
  keeps its own process-wide pool, so the three RawDeveloper workers can
  each call it; use `threads = max(1, cores / 2)` per call so two concurrent
  develops share the machine.

## Which level to decode

| request | mosaic needed | libcrx call |
|---|---|---|
| fit-to-screen on the 3024-px display, index renders | 2x2-binned RGB of the full plane (today's `halfSize`) | level 0, then bin as today; or level 1 mosaic demosaiced (61% fewer coded bytes, about 6 ms) |
| 100% zoom, 6K displays | full mosaic | level 0 |
| strip thumbnails, stacks | quarter | level 2 (3.5 ms), or the preview track |

The level-1 path needs `RawDevelop` to accept a mosaic that is already
half the sensor size and demosaic it (Malvar-He-Cutler exists for
`fullSize`). Measure both against the current binned look before choosing;
the plan's default is level 0 + bin, which is a pure swap.

## Background self-check

For the first weeks, one develop in a hundred (deterministic: hash of the
path) also runs `libraw_unpack` and compares the two mosaics with
`memcmp`. A difference is written to a log in Application Support with the
file's SHA-256 and the first differing site, and the develop uses LibRaw's
result. The log is read at launch and surfaced in the self-test. Silent
for a week on Kamen's use means the fallback can be reduced to
unsupported variants only.

## What to measure before merging (side instance first, as always)

- `--raw-spam-test` and `--index-test` batteries against the current main:
  develops per second, open-to-screen medians, index time for 1500 files.
- The self-check counter: zero differences.
- Memory: peak RSS during an index run (libcrx keeps ~140 MB of workspace
  per concurrent decode in the cache; three workers means ~420 MB
  resident while indexing. If that is too much for 8 GB machines, the
  cache slot count is one constant.)

## Not in step 1

Own metadata parsing (black level, WB, matrix from the makernotes) to drop
LibRaw entirely; the Linux port needs it eventually and LibRaw's CDDL
build works there too. Preview-track thumbnails. Level-1 demosaic path.
