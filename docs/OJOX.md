# Integration into OjoX (milestone 8)

## Status and direction, 2026-10-12

- **A prototype exists:** OjoX branch `crx`, four commits from 2026-09-09.
  It is step 1 below: LibRaw opens the file for metadata, libcrx unpacks, and
  the self-test finds the developed pictures byte-identical to LibRaw's (60 of
  60). In the app the whole half-size develop went from about 46 ms to about
  31 ms. It sits on a main hundreds of commits old and is rebuilt, not merged.
- **Kamen's direction (2026-10-01, `DECISIONS.md`):** long-term, no merge before
  about two months of testing, and always an opt-in experiment, never the
  default. The branch lives on OjoX's latest main and is rebased whenever main
  moves.
- **What the rebuild changes from the prototype:**
  - Off by default, behind a switch. Switched off, the app runs main's code
    and nothing else; the prototype was on whenever RAW decoding was.
  - Every decode is checked against LibRaw in the background during the
    testing period, not one in a hundred: libcrx's develop is shown at once,
    LibRaw unpacks the same file when the machine is idle, and a difference
    replaces the picture with LibRaw's, drops the cached copy and is logged.
  - Live RAW while stepping only when LibRaw is the chosen engine: the
    prototype ignored Apple's, and with it selected a held key would ask for
    a slow Apple develop on every step.
  - The develop it feeds is main's, which since `crx-prep` balances white by
    the smallest multiplier and clips blown highlights in a table. The
    prototype's own copy of that loop does not clip; a rebase keeps main's.
- **End-to-end speed:** the level-0 swap gives the app about 1.5x, because
  LibRaw still opens every file and the develop still runs. Getting closer to
  the decoder's own 43 against 5 ms needs the level-1 path below and reading
  the metadata ourselves.


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
