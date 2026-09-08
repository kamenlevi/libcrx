# The CRX format, as libcrx understands it

Every rule the decoder uses lives here first. A section is complete when it
has the rule, its source, a hand-worked example small enough to check on
paper, and the name of the test that encodes that example. Code with no
section here does not merge.

Sources, in order of authority:

1. Real files: the private corpus (15,307 files from an EOS R and an EOS R6
   Mark II) and the public raw.pixls.us samples, read with our own tools.
2. Laurent Clévy, *canon_cr3* (github.com/lclevy/canon_cr3): the container,
   the CMP1 box, the tile, plane and subband headers.
3. Canon, US 2016/0323602 A1: the three-level 5/3 wavelet, adaptive Rice
   coding with run mode, the subband order.
4. The reference decoders (LibRaw, rawspeed) as black boxes. Their
   behaviour on real files fixes the details the documents leave open. Their
   source is consulted to understand behaviour, never copied; every rule
   below is restated from scratch and verified against files.

Notation. All quantities are integers. `>>` on a signed value is an
arithmetic shift (rounds toward minus infinity); `/` is C division (rounds
toward zero) and is only used where the format really does that. `ceil2(n)`
is `(n + 1) >> 1`, `floor2(n)` is `n >> 1`. Multi-byte fields are big-endian
unless stated. Bit fields are numbered from the most significant bit.

## 0. What the codec is, in one paragraph

A CR3 is an ISO base media file. One of its tracks holds the sensor image
encoded with Canon's CRX codec. The image is split into up to four colour
planes (one per position of the 2x2 Bayer cell), each plane into one or two
side-by-side tiles. Each tile plane is either coded directly (lossless
"RAW") or first transformed by three levels of an integer 5/3 wavelet, the
coefficients quantised, and then coded (lossy "C-RAW"). In both cases the
symbol coder is the same adaptive Golomb-Rice coder with a run mode and,
for the base band, median-edge prediction. Every subband of every plane of
every tile is an independent bitstream with its own header. Nothing in the
format is floating point.

## 1. Container

The boxes on the path to the image, in file order:

    ftyp
    moov
      uuid 85c0b687-820f-11e0-8111-f4ce462b6a48     Canon metadata (CNCV, CCTP, CTBO, ...)
      trak                                          track 1: full-size JPEG
      trak                                          track 2: small CRX (1624x1080-class)
      trak                                          track 3: full-size CRX  <- the one we decode
      trak                                          track 4: timed metadata
      [trak]                                        track 5, some bodies: dual-pixel CRX
    mdat

Inside each CRX track: `mdia/minf/stbl/stsd/CRAW` is the sample entry
(82 bytes of fields, then child boxes) and `CRAW/CMP1` is the codec header.
`stbl/stsz` gives the sample size: if its `sample_size` field is nonzero
that is the size, otherwise the first entry of the following table is (the
R6 Mark II writes it that way). `stbl/co64` (or `stco`) gives the absolute
file offset of the sample. `CRAW` bytes 24..27 hold the width and height
Canon reports for the track, which can be smaller than CMP1's (the R6 Mark
II says 6000x4000 while the coded image is 6188x4120).

Rule: the image track is the CRX track whose CMP1 image area is largest.
Every file seen has exactly one CMP1 track of each of two sizes.

Test: `test_container` (a synthetic file built by `tests/mkcr3.h`).

## 2. CMP1: the image header

Offsets from the start of the box payload (after the 8-byte box header):

| offset | size | field | seen |
|---|---|---|---|
| 4 | u16 | codec version | 0x100 (EOS R, all pixls bodies), 0x200 (EOS R6 Mark II) |
| 8 | u32 | image width W | 6888, 6188, 6288, 8352, ... |
| 12 | u32 | image height H | |
| 16 | u32 | tile width TW | W (one tile) or W/2 (two tiles) |
| 20 | u32 | tile height TH | H |
| 24 | u8 | bit depth B | 14 |
| 25 | u4,u4 | plane count P, CFA layout | 4; layout 0 (RGGB) for big images, 1 (GRBG) for the small track of some bodies |
| 26 | u4,u4 | encoding type, wavelet levels N | type 0; N = 0 (RAW) or 3 (C-RAW) |
| 27 | bit7, bit6 | more than one tile across, more than one tile down | across: 1 on lossy two-tile files, but 0 on lossless two-tile files (EOS M50, 250D, 850D); the decoder ignores the bit and derives the tile count from TW and W. Down: always 0 |
| 28 | u32 | codestream header size S | 0x70 .. 0x438 (v1), 0x368 (v2) |
| 32 | bit7 | extended header present | 0 |

With P = 4 every plane is W/2 x H/2 and every tile plane is TW/2 x TH/2;
W, H, TW and TH are even. Tile columns = ceil(W / TW), tile rows =
ceil(H / TH); the last column is W - TW * (cols - 1) wide.

Values seen in no file, and therefore outside milestones 2 to 7: P = 1,
encoding types 1 and 3, the extended header, N other than 0 or 3, tile rows
> 1, B other than 14. The decoder returns `CRX_E_UNSUPPORTED` for them
rather than guessing (section 11).

Worked example: bytes at offset 24 of the R6 Mark II header are `0e 40 03 00`:
B = 14, P = 4, layout 0, type 0, N = 3, no extra tile columns.

Test: `test_container` and the corpus headers check.

## 3. Codestream headers

The sample in `mdat` starts with S bytes of headers, then the subband data
back to back in header order. Three header kinds, each `marker(u16)
length(u16) payload`:

Tile, marker `ff01` (v1, length 8) or `ff11` (v2, length 8 or 16):

| offset | field |
|---|---|
| 4 | u32 tile data size (sum of its planes) |
| 8 | u16 tile index (0, 1) |
| 10 | u16 0 (v1) / 0x4000 (v2 with length 16) |
| 12 | u32 QP table size in bytes (v2, length 16 only) |
| 16 | u16 extra bytes after the QP table (v2, length 16 only; seen 5, 6) |
| 18 | u16 0 |

Plane, marker `ff02` / `ff12`, length 8:

| offset | field |
|---|---|
| 4 | u32 plane data size (sum of its subbands) |
| 8 | bits 7..4 plane index 0..3; bit 3 "supports partial" (seen 1); bits 2..1 rounded bits (seen 0) |
| 9 | 3 bytes 0 |

Subband, marker `ff03` (v1, length 8) or `ff13` (v2, length 16):

| offset | v1 | v2 |
|---|---|---|
| 4 | u32 subband size in bytes | same |
| 8 | u32 packed: bits 31..28 subband index 0..9; bit 27 partial (seen 0); bits 26..19 q parameter; bits 18..0 unused tail bits (0..7) | byte 8 bits 7..4 subband index; low 12 bits of u16 at 8 must be 0 |
| 10 | | u16 quantisation multiplier ("qmult", seen 0, 4, 8, 16) |
| 12 | | u32 quantisation base ("qbase", seen 0, 1) |
| 16 | | u16 unused tail bits (0..7) |
| 18 | | u16 0 |

The tail field records padding at the end of the subband's bytes. The
reference, and therefore libcrx, treats it as a byte count: the coder may
read `data_size = size - tail` bytes, and any bit requested beyond that
reads as 0 (section 5.1). Every observed value is 0..7.

Subband order within a plane, N = 3: `LL3, HL3, LH3, HH3, HL2, LH2, HH2,
HL1, LH1, HH1` (index 0..9). HL means high-pass horizontally, low-pass
vertically. N = 0: one subband, index 0.

The q parameter (v1) per subband index in every corpus file: 4, 4, 4, 4,
16, 16, 22, 26, 26, 32. The v2 (qbase, qmult) per index: (1,0) x4, (0,4),
(0,4), (0,8), (0,8), (0,8), (0,16).

After the last subband header of the last plane of the last tile come 4
zero bytes; S counts them.

Data layout in the sample: tile 0's data starts at offset S. In v2 a tile's
data begins with its QP table (`QP size` bytes) followed by `extra` bytes,
then the planes. Plane data follows plane data, subband follows subband,
with no gaps, in header order.

Test: `test_container` (v1 and v2 synthetic headers) and the corpus headers check.

## 4. Geometry: tiles, plane sizes, subband sizes

Let a tile plane be `w x h` (for the EOS R: 1722 x 2273; R6 Mark II:
3094 x 2060). Tile flags: `LEFT` if a tile exists to the left, `RIGHT` if
to the right (and, in principle, `TOP`, `BOTTOM`).

### 4.1 Base counts

One level of the 5/3 wavelet on a line of n samples gives `ceil2(n)`
low-pass and `floor2(n)` high-pass coefficients (even positions are
low-pass). Level 1 acts on the plane, level 2 on its LL, level 3 on LL2:

    n_0 = w,  n_l = ceil2(n_{l-1})            widths, same for heights

    LL_N : ceil2(n_{N-1}) x ceil2(m_{N-1})
    HL_l : floor2(n_{l-1}) x ceil2(m_{l-1})
    LH_l : ceil2(n_{l-1}) x floor2(m_{l-1})
    HH_l : floor2(n_{l-1}) x floor2(m_{l-1})

### 4.2 Extra coefficients at a tile seam

A tile that has a neighbour on the right (or below) carries, in every
subband, extra columns (rows) of coefficients belonging to the neighbour,
so that the wavelet can be inverted across the seam without mirroring. A
tile with a neighbour on the left carries exactly one extra column in every
horizontally high-pass band (HL, HH) at every level, placed before column 0.
No extra low-pass columns are stored on the left.

The number of extra columns on the right at each level is the least fixed
point of three rules (rows are the same with heights):

1. **Need.** To reconstruct samples 0..m-1 of a line whose continuation
   exists, the synthesis needs `L = H = ceil2(m)` coefficients if m is odd
   and `L = H = m/2 + 1` if m is even. (Sample 2i needs l[i], h[i-1], h[i];
   sample 2i+1 needs those plus l[i+1], h[i+1].)
2. **Produce.** A synthesis stage that is not the finest produces every
   sample its coefficients allow: `2 * min(L, H) - 1` samples, which become
   the low-pass line of the next finer level. The finest stage produces
   exactly w samples.
3. **Chain.** The samples a stage must produce are at least the low-pass
   count the finer stage needs.

Iterating from "no extras" converges in three rounds. Written out for a
width w with `w mod 8 = r`, the extras (right side; `H` for HL/HH, `L` for
LH and, at the coarsest level, LL) are:

| N | r | level 1 (finest) | level 2 | level 3 (coarsest) |
|---|---|---|---|---|
| 1 | even | H+1 L+1 | | |
| 1 | odd | H+1 L+0 | | |
| 2 | 0,4 | H+1 L+1 | H+1 L+1 | |
| 2 | 1,5 | H+1 L+0 | H+1 L+0 | |
| 2 | 2,6 | H+1 L+2 | H+2 L+1 | |
| 2 | 3,7 | H+1 L+1 | H+1 L+1 | |
| 3 | 0 | H+1 L+1 | H+1 L+1 | H+1 L+1 |
| 3 | 1 | H+1 L+0 | H+1 L+0 | H+1 L+0 |
| 3 | 2 | H+1 L+2 | H+2 L+2 | H+2 L+1 |
| 3 | 3 | H+1 L+1 | H+1 L+2 | H+2 L+1 |
| 3 | 4 | H+1 L+1 | H+1 L+2 | H+2 L+1 |
| 3 | 5 | H+1 L+0 | H+1 L+1 | H+1 L+1 |
| 3 | 6 | H+1 L+2 | H+2 L+1 | H+1 L+1 |
| 3 | 7 | H+1 L+1 | H+1 L+1 | H+1 L+1 |

(The finest-level LL extra is not a stored quantity; LL is only stored at
the coarsest level.)

Worked example, N = 2, w = 10, neighbour on the right. Round 1: stage 1
must produce 10 samples, needs L = H = 6; stage 2 must produce 5 (base) and
needs L = H = 3, and produces 2*3-1 = 5. Round 2: stage 1 needs 6 low-pass
samples from stage 2, which must now produce 6: needs L = H = 4 and
produces 7. Round 3: stage 2 producing 7 still needs L = H = 4: fixed.
So HL1 is 6 wide (5+1), LH1 7 wide (5+2), HL2 4 wide (2+2), LL2 4 wide
(3+1), matching the table's row `2 | 2,6`.

Worked example, the EOS R: w = 1722 (r = 2), N = 3, tile 0 has RIGHT, tile
1 has LEFT. Tile 0: HL1 861+1, LH1 861+2, HH1 861+1 wide; HL2 431+2, LH2
431+2, HH2 431+2; HL3 216+2, LH3 216+1, HH3 216+2; LL3 216+1. Tile 1: HL
and HH bands +1 on the left, LH and LL unchanged.

Bands with a seam on the left index their first stored column as -1; the
decoder's line buffers are one wider on that side.

Test: `test_band_geometry` (the rule against the 48 (N, r) cases and
against the EOS R and R6 Mark II header sizes: each subband's symbol count
must be reachable from its byte size, and the harness proves the sizes).

## 5. The symbol coder

Every subband is one bitstream. Bits are read from each byte most
significant bit first, bytes in order.

### 5.1 Primitives

- `zeros()`: count consecutive 0 bits up to and including the next 1 bit;
  return the count (the 1 is consumed).
- `bits(n)`: the next n bits as an unsigned integer, 0 <= n <= 21.
- Past the end of the subband's `data_size` bytes every bit reads as 0.
  The decoder counts such bits; the corpus check asserts the count is 0 on
  every file, so that leniency is observable, never silent.

### 5.2 Rice code with escape

    code(k):
        q = zeros()
        if q >= 41:       return bits(21)            escape: raw 21-bit value
        if k == 0:        return q
        return (q << k) | bits(k)

Worked examples (bits shown left to right):

| k | bits | q | code |
|---|---|---|---|
| 0 | `1` | 0 | 0 |
| 2 | `001 10` | 2 | (2<<2) \| 2 = 10 |
| 1 | `0000001 0` | 6 | (6<<1) \| 0 = 12 |
| any | 41 zeros, `1`, then 21 bits | 41 | those 21 bits |

### 5.3 Sign mapping

    signed(code) = code >> 1        if code is even
                 = -(code >> 1) - 1 if code is odd

so codes 0, 1, 2, 3, 4 mean 0, -1, +1, -2, +2. Branch-free form:
`(code >> 1) ^ -(code & 1)`.

### 5.4 Rice parameter adaptation

    adapt(k, c, kmax):
        k' = k - [c < 2^(k-1)] + [(c >> k) > 2] + [(c >> k) > 5]
        return min(k', kmax)           (with 2^(k-1) read as 0 when k = 0)

k never goes below 0 (the subtraction only fires when k >= 1) and never
above kmax. kmax is 15 for image bands and 7 for QP tables (section 7.2).

Examples: adapt(2, 10, 15) = 2 (10 >= 2, 10>>2 = 2). adapt(1, 12, 15) = 1 -
0 + 1 + 1 = 3 (12>>1 = 6 > 5). adapt(0, 0, 15) = 0.

In the interior of a line the value fed to `adapt` is not the code alone
but a blend with the local gradient of the previous line (5.6, 5.7).

### 5.5 Run mode

Two adaptive tables indexed by the run state s (0..31):

    JS[s] = 1,1,1,1, 2,2,2,2, 4,4,4,4, 8,8,8,8, 16,16, 32,32, 64,64, 128,128,
            256, 512, 1024, 2048, 4096, 8192, 16384, 32768
    J[s]  = 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4, 5,5, 6,6, 7,7,
            8, 9, 10, 11, 12, 13, 14, 15

(JS[s] = 2^J[s]; J rises by one every four states up to 3, every two
states up to 7, then every state.)

    run(remaining):                     remaining = symbols left in the line
        if bits(1) == 0: return 0
        n = 1
        while bits(1) == 1:
            n += JS[s]
            if n > remaining: n = remaining; break
            if s < 31: s += 1
            if n == remaining: break
        if n < remaining:
            if J[s] > 0: n += bits(J[s])
            if s > 0: s -= 1
            if n > remaining: error (corrupt)
        return n

s starts at 0 for every subband and persists across lines. Note the two
`break`s skip the "n < remaining" block, so a run that reaches the end of
the line neither reads J bits nor decrements s.

Worked example, s = 0, bits `1 1 1 0`: start (n=1); 1: n = 2, s = 1;
1: n = 3, s = 2; 0: stop; J[2] = 0, s -> 1. Run of 3, s = 1 afterwards.
Worked example, s = 8 (JS 4, J 2), bits `1 1 0 11`: n = 1; 1: n = 5,
s = 9; 0: stop; J[9] = 2: n += 3 = 8; s -> 8. Run of 8.

Test: `test_rice` (5.2 to 5.5 examples, plus a streaming check that a
`zeros()` spanning byte boundaries and a 21-bit escape decode correctly).

### 5.6 Base-band lines (LL, and the only band of a lossless plane)

State per band: k (0..15), s (0..31), the previous line `p[]` and the
current line `c[]`, each with one pad element on the left and one on the
right. For pixel x (0-based) write

    a = c[x-1]   left        b = p[x]   top
    cc = p[x-1]  top-left    d = p[x+1] top-right

**Median-edge prediction** (JPEG-LS MED):

    med(a, b, cc) = min(a, b)   if cc >= max(a, b)
                    max(a, b)   if cc <= min(a, b)
                    a + b - cc  otherwise

Branch-free form used by the reference, proven equal by case analysis on
the sign of (b - cc) and the orders of a, b, cc:

    t = b - cc
    i = ((cc < a) xor (t < 0)) * 2 + ((a < b) xor (t < 0))
    pred = [a + t, a + t, a, b][i]

Examples: a=10 b=12 cc=11 -> 11 (a+b-cc). a=10 b=12 cc=13 -> 10 (min).
a=12 b=10 cc=9 -> 12 (max).

**Symbol** with prediction `pred`, at pixel x, with `more` true when x is
not the last pixel:

    v = code(k)
    c[x] = pred + signed(v)
    if more:  v = (v + |2 * (d - b)|) >> 1        gradient blend, uses p[x+1] - p[x]
    k = adapt(k, v, 15)

**Line** (not the first line of the band):

    c[-1] = p[0]                              left pad = first top value
    x = 0
    while x < width:
        if x == width - 1:
            symbol(med(a, b, cc), more = false); break
        if not (a == b and a == d):
            symbol(med(a, b, cc), more = true); x += 1
        else:
            n = run(width - x)
            c[x .. x+n-1] = a;  x += n            (a is c[x-1], unchanged by the copies)
            if x < width:
                symbol(pred = b, more = (x < width - 1)); x += 1
    c[width] = c[width - 1] + 1                 right pad, kept for fidelity

Run mode is entered when left, top and top-right are all equal; the symbol
after a run is predicted from the top pixel alone (no MED); and the last
pixel always goes through the MED path.

**First line** of the band: there is no p[]. Prediction is the left pixel
(0 before the first pixel), run mode is entered whenever the left pixel is
0, no gradient blend:

    c[-1] = 0; x = 0
    while x < width:
        if a == 0:
            n = run(width - x); c[x .. x+n-1] = 0; x += n
            if x == width: break
        v = code(k); c[x] = a + signed(v); k = adapt(k, v, 15); x += 1
    c[width] = c[width - 1] + 1

(When a != 0 the run test is skipped; when a == 0 and the run bit is 0, n
is 0 and a symbol follows with prediction 0.)

Worked example: width 4, first line, k = 0, s = 0, bits
`1 1 0 | 0 1 | 1 | 0 0 1`: a=0 -> run: `1` start n=1, `1` n=2 s=1, `0` stop,
J[1]=0, s=0 -> run 2: c = [0, 0, ?, ?]. x=2: a=0 -> run bit `0`: n=0.
symbol: code(0): zeros `1` -> 0, c[2] = 0, k stays 0 (adapt(0,0)). x=3:
a=0 -> run bit `1`... (the example continues in the test with the exact
bit string and expected line `[0, 0, 0, -1]`).

Test: `test_line_ll` (hand-built bitstreams for a 4-wide, 3-line band,
including a run that reaches the line end).

### 5.7 High-pass band lines (every band except the base band)

No prediction: each symbol is the coefficient itself. Run mode encodes runs
of zeros. The Rice parameter adapts with a per-column memory `kp[]`, the k
that was in force after each column of the previous line (0 above the first
line, and 0 for columns that were inside a run).

    symbol_hf(x, shifted):
        v = code(k)
        c[x] = signed(v + 1) if shifted else signed(v)
        k = adapt(k, v, none)                        no clamp yet
        if kp[x+1] - k <= 1: k = min(k, 15)          kp[x+1]: memory of the next column
        else:                k = k + 1
        kp[x] = k

The last pixel of a line uses the plain rule `k = adapt(k, v, 15)` and
sets `kp[x] = k`.

    line_hf:
        x = 0
        while x < width - 1:
            if p[x+1] != 0 or p[x] != 0 or c[x-1] != 0:        top-right, top, left
                symbol_hf(x, shifted = false); x += 1
            else:
                n = run(width - x)      (with the same tables and s; a run may not exceed width - x)
                c[x .. x+n-1] = 0; kp[x .. x+n-1] = 0; x += n
                if x == width - 1: last pixel: v = code(k); c[x] = signed(v + 1);
                                   k = adapt(k, v, 15); kp[x] = k; x += 1
                elif x < width - 1: symbol_hf(x, shifted = true); x += 1
        if x == width - 1:
            v = code(k); c[x] = signed(v); k = adapt(k, v, 15); kp[x] = k

The `shifted` form after a run reflects that the value cannot be zero (a
zero would have extended the run), so code 0 means the smallest nonzero
magnitude. Note `c[-1]` (left pad) is 0 at the start of every line and
`p[width]` (right pad of the previous line) is never consulted, because the
loop stops at width - 1.

Bound on k: by induction every stored kp is <= 15 (first line stores
clamped values; later lines store min(k, 15) or kp[x+1] - 1), so the k used
to read the next code is always in 0..15.

**First line** of a high-pass band: `p[]` and `kp[]` are all 0, which makes
the rule above degenerate to: run mode whenever the left pixel is 0; after
a run, the shifted symbol with `k = adapt(k, v, 15)`; otherwise the plain
symbol with `k = adapt(k, v, 15)`; `kp[x] = k` after every pixel.

Test: `test_line_hf` (hand-built 5-wide, 3-line band exercising the
memory rule, the shifted symbol and a run that ends the line).

## 6. Rounded-bits and per-subband partial modes

The plane header's rounded-bits field and the v1 subband header's partial
flag select decoder variants (coarser value steps in a lossless plane;
per-line q updates) that appear in no file of either corpus. libcrx returns
`CRX_E_UNSUPPORTED` when either is set. Recorded here so the gap is known,
not forgotten.

## 7. Quantisation

Coefficients of lossy bands are multiplied back by an integer step before
synthesis. The base band of a lossy plane has step 1 in every observed file
(v1 q = 4; v2 qbase = 1, qmult = 0). Lossless planes have no quantisation.

### 7.1 The step table

    T[i] = ceil(40 * 2^(i/6)),  i = 0..5  =  40, 45, 51, 57, 64, 72

    step(q) = T[q mod 6] >> (6 - q div 6)          for q div 6 < 6
            = T[q mod 6] << (q div 6 - 6)          otherwise (never observed; q <= 32)

so step doubles every 6 and step(4) = 64 >> 6 = 1. Observed: step(16) = 4,
step(22) = 8, step(26) = 12, step(32) = 25.

### 7.2 Version 1: one q per subband

The q parameter is in the subband header (section 3). Every coefficient of
the band is multiplied by step(q). (With the partial flag set the q would be
updated at each line from a Rice-coded delta; unsupported, section 6.)

### 7.3 Version 2: a QP map per tile

The tile's data begins with a QP table coded with the same Rice coder:
`qw = ceil(w / 8)` values per row, `qh = ceil(h / 2)` rows (w, h the tile
plane size). Escape threshold is 23 with an 8-bit raw value instead of 41
and 21 bits; kmax is 7; there is no run mode:

    code_qp(k): q = zeros(); if q >= 23: return bits(8); if k == 0: return q; return (q << k) | bits(k)

    first row:   left = 0; for x: v = code_qp(k); r[x] = left + signed(v); k = adapt(k, v, 7); left = r[x]
    other rows:  r[-1] = p[0]  (left pad = first top value)
                 for x: pred = med(a, b, cc) where cc = p[x-1], b = p[x], a = r[x-1]
                        v = code_qp(k); r[x] = pred + signed(v)
                        if x < qw - 1: k = adapt(k, (v + 2 * |p[x+1] - p[x]|) >> 1, 7)
                        else:          k = adapt(k, v, 7)
    qp[y][x] = r[x] + 4

k starts at 0 and persists through the table. The `med` here is the same
MED as 5.6, written in the reference with the arguments in a different
order; the test checks both agree.

From the map, one step table per wavelet level:

    level 1 (finest):   S1[y][x] = step(qp[y][x]),            qh rows
    level 2:            S2[y][x] = step(trunc((qp[2y][x] + qp[2y+1][x]) / 2)),   ceil(h/4) rows
    level 3:            S3[y][x] = step(trunc((qp[4y][x] + ... + qp[4y+3][x]) / 4)), ceil(h/8) rows

Rows past the end of the map repeat the last row; `trunc` is C division
(toward zero), which differs from `>>` only for negative sums; the decoder
uses C division to match. `step()` on a negative value is undefined in the
reference; the decoder treats a negative average as corrupt. (No corpus
file produces one; the harness counts.)

Per subband at level l with header (qbase, qmult), the multiplier of the
coefficient at band row y, band column x is

    m = clamp(qbase + ((S_l[ry][rx] * qmult) >> 3), 1, 0x168000)

where the map cell is found from the coefficient position inside the
tile's own area (the extra seam columns and rows map to the nearest edge
cell): with `sh = 3 - l` (2, 1, 0 for levels 1, 2, 3)

    rx = clamp((x - left_extra) >> sh, 0, width_of_S - 1)   with x counted from the first stored column
    ry = clamp(y - top_extra, 0, own_rows - 1) >> 0 ... (rows are already at band resolution: one map row per band row at level 1; the level-2 and level-3 maps are built at their bands' resolutions)

Precisely: a band row y < top_extra uses map row 0; a row in the tile's own
area uses map row (y - top_extra); a row in the bottom extra area uses the
last own row. Columns: the first `left_extra` columns use map column 0;
own columns use `(x - left_extra) >> sh`; right extra columns use the map
column of the last own column.

Observed qmult per band index: 0 (LL3 and level 3), 4 (level 2), 8 (HL1,
LH1), 16 (HH1), so with qp = 26: m = 1, 6, 12, 24 respectively.

Test: `test_qp` (a 3x3 QP map built by hand through the coder, the three
level tables, and the multiplier lookup at a seam).

## 8. The wavelet

### 8.1 One-dimensional synthesis (Le Gall 5/3, integer lifting)

Given low-pass `l[0..nl-1]` and high-pass `h[0..nh-1]` for a line of m
samples (even positions low-pass):

    x[2i]   = l[i] - ((h[i-1] + h[i] + 2) >> 2)
    x[2i+1] = h[i] + ((x[2i] + x[2i+2]) >> 1)

Boundaries when no neighbour exists (mirror extension, h[-1] = h[0],
x[m] = x[m-2]):

    x[0]   = l[0] - ((h[0] + 1) >> 1)
    m even: x[m-1] = h[nh-1] + x[m-2]
    m odd:  x[m-1] = l[nl-1] - ((h[nh-1] + 1) >> 1)
    m == 1: x[0] = l[0]

Boundaries at a tile seam use the stored neighbour coefficients instead:
on the left, `h[-1]` is the extra column; on the right, `h[nh]` (and, for
even m, `l[nl]`) are the extra columns, so the interior formula applies at
the edge. Which extras exist is fixed by section 4.2.

This is exactly invertible. The analysis that the encoder ran is

    h[i] = x[2i+1] - ((x[2i] + x[2i+2]) >> 1)
    l[i] = x[2i]   + ((h[i-1] + h[i] + 2) >> 2)

and analysis(synthesis(l, h)) = (l, h) for every integer input, because each
lifting step adds a function of the other parity and is undone by
subtracting the same function. The low-pass has unit DC gain (a constant
line gives h = 0, l = x).

Worked example: l = [10, 12, 11], h = [1, -2], m = 5, no neighbours:
x0 = 10 - ((1+1)>>1) = 9; x2 = 12 - ((1-2+2)>>2) = 12; x1 = 1 + ((9+12)>>1)
= 11; x4 = 11 - ((-2+1)>>1) = 11 - (-1) = 12; x3 = -2 + ((12+12)>>1) = 10.
x = [9, 11, 12, 10, 12]. Analysis of x returns h = [1, -2], l = [10, 12, 11].

### 8.2 Two dimensions

Each level: rows first, then columns, or the reverse; the integer lifting
is separable and the two orders give identical results because each 1-D
transform is applied to whole lines independently. Reconstruction of one
level from `LL, HL, LH, HH`:

    for each row r:  A[r] = synth_1d(LL[r], HL[r])      the "low" rows (even output rows)
                     B[r] = synth_1d(LH[r], HH[r])      the "high" rows (odd output rows)
    for each column: X[.][c] = synth_1d(A[.][c], B[.][c])   with the vertical boundary rules

The vertical boundaries follow 8.1 with TOP/BOTTOM neighbours (extra rows).
Tile rows never exceed 1 in any file, so only mirror rules apply
vertically; the seam rules are implemented symmetrically anyway and tested
on synthetic data.

The reference evaluates this in a rolling window of a few rows to save
memory; the values are the same. libcrx may use any schedule.

### 8.3 Three levels

    LL2 = synth(LL3, HL3, LH3, HH3)      size ceil2 x ceil2 of LL1's size ...
    LL1 = synth(LL2, HL2, LH2, HH2)
    P   = synth(LL1, HL1, LH1, HH1)      the tile plane, w x h

At a seam the intermediate LL2 and LL1 lines are wider than base (section
4.2, rule "produce"), and the finer stage uses as many of them as it needs.

Test: `test_wavelet` (the 8.1 example; a random 37x23 integer image,
analysed by the reference transform in `tests/ref_wavelet.c`, synthesised
by the decoder, compared exactly; the same across a synthetic seam).

## 9. From coefficients to sensor values

For plane index p in 0..3 and tile plane sample P[y][x] (after full
synthesis, or directly from the base band when N = 0):

    value = clamp(2^(B-1) + P[y][x], 0, 2^B - 1)            B = 14: 8192 + P, clamped to 0..16383

Output positions in the W x H mosaic, tile origin (tx, ty) in plane units:

    layout 0 (RGGB):  plane 0 -> (2y, 2x)   1 -> (2y, 2x+1)   2 -> (2y+1, 2x)   3 -> (2y+1, 2x+1)
    layout 1 (GRBG):  plane 0 -> (2y, 2x+1) 1 -> (2y, 2x)     2 -> (2y+1, 2x+1) 3 -> (2y+1, 2x)
    layout 2 (GBRG):  plane 0 -> (2y+1, 2x) 1 -> (2y+1, 2x+1) 2 -> (2y, 2x)     3 -> (2y, 2x+1)
    layout 3 (BGGR):  plane 0 -> (2y+1, 2x+1) 1 -> (2y+1, 2x) 2 -> (2y, 2x+1)   3 -> (2y, 2x)

(as row, column offsets within the 2x2 cell; plane 0 is red, 1 and 2 green,
3 blue, and the layout says where red sits). The oracle's `filters` value
0xb4b4b4b4 for both corpus cameras is LibRaw's code for RGGB, consistent
with layout 0.

The output is `uint16` per site. Nothing else (no black subtraction, no
scaling) is applied.

Test: `test_output` (a 2x2-plane synthetic image through all four
layouts; clamping at both ends).

## 10. Partial decode

`crx_decode(level = n)` for 1 <= n <= N returns the mosaic assembled from
each plane's `LL_n` as it stands after synthesis has run from level N down
to level n+1, i.e. before the last n synthesis stages, converted by section
9 with the same offset and clamp. For n = N it is the dequantised base band
itself.

Definition, in one line: **level-n output = the low-pass band of the exact
integer 5/3 analysis, applied n times, of the unclamped level-0 plane.**
This holds by 8.1 (analysis inverts synthesis exactly) and needs no
approximation. Because the low-pass has unit DC gain the values live on the
same scale as the pixels, so the offset and clamp of section 9 apply
unchanged. Clamping happens after the analysis, so a level-n sample can
differ from what a caller would get by analysing the clamped level-0 image
at sites that clip; that is the intended definition and the tested one.

Geometry: plane size at level n is `ceil2` applied n times to w and h per
tile, tiles concatenated; the mosaic is twice that. Seam extras are
dropped. For N = 0 files only level 0 exists; a viewer bins.

Cost: the bytes of the bands that a level-n decode does not read, in the
corpus (share of the plane's coded size, mean over 40 R6 Mark II files):

| skipped | HL1 LH1 HH1 | + HL2 LH2 HH2 | + HL3 LH3 HH3 |
|---|---|---|---|
| level | 1 | 2 | 3 |
| bytes not read | 61% | 84% | 96% |

Test: `test_partial` (decoder at level 1 and 2 against the reference
analysis of the decoder's own unclamped level-0 output, on the synthetic
image; the harness repeats it on real files).

## 11. Bit widths and overflow

| quantity | range on conforming input | storage | why |
|---|---|---|---|
| Rice code | 0 .. 2^21 - 1 | u32 | escape is 21 bits; non-escape (q < 41) << 15 fits |
| signed symbol | -2^20 .. 2^20 - 1 | i32 | half the code |
| base-band pixel P | -2^13 .. 2^13 - 1 nominal; up to +-2^21 decodable | i32 | prediction + symbol; MED of i32 |
| gradient blend `v + \|2(d-b)\|` | < 2^23 | u32 | 2^21 + 2^22 |
| coefficient x step | < 2^21 x 0x168000 < 2^42 formally; < 2^17 x 144 < 2^25 on real files | i32 | see below |
| lifting sums | 4 terms of the above | i32 | < 2^27 on real files |
| output | 0 .. 2^B - 1 | u16 | clamp |

Conforming streams (those an encoder produced from B-bit sensor data) keep
every intermediate far inside i32: a 14-bit plane through three analysis
levels has coefficients below 2^17 in magnitude (each level grows the
high-pass by at most a factor 2 and the low-pass not at all), quantisation
only shrinks them, and the observed multipliers are at most 144. The
decoder therefore computes in i32 with two's-complement wrap on overflow
(compiled with `-fwrapv`), so that its behaviour on hostile input is
defined and deterministic, and the fuzzer checks it never reads or writes
outside its buffers. It does not promise to match the reference on
overflowing input, because the reference's behaviour there is undefined C.

## 12. Variants in the corpora, and scope

| body | codec | mode | tiles | files |
|---|---|---|---|---|
| EOS R6 Mark II (private) | v2 | C-RAW, N=3, QP maps | 1 | 12,306 |
| EOS R (private) | v1 | C-RAW, N=3 | 2 | 3,001 |
| small tracks of the above (1624x1080) | as parent | C-RAW | 1 | 15,307 |
| pixls: M50, Kiss M, M50 II, 250D, SL3, 850D, 200D II, M6 II, 90D, R, RP, R5, R6, R3, R7, R10, 1DX III, SX70, G5X II, G7X III | v1 (v2 for the 1DX III C-RAW per Clévy) | RAW (N=0) and C-RAW | 1 and 2 | 110 |

Everything in that table is milestone 2 to 5 scope. Encoding types 1 and
3, single-plane images, rounded bits, per-subband partial q, more than one
tile row, and bit depths other than 14 are returned as `CRX_E_UNSUPPORTED`
and are listed in DECISIONS.md as open.

## 13. Test index

| test | sections | status |
|---|---|---|
| test_container | 1, 2, 3 (synthetic v1 lossless and v2 lossy files, truncations, a size that does not add up) | done |
| test_band_geometry | 4 (the rule against the observed table for widths 22..3999, the worked example) | done |
| crxcheck -H on the corpora | 1 to 4 on real files | done: 15,307 + 64 files |
| test_rice | 5.1-5.5 | planned (M3) |
| test_line_ll | 5.6 | planned (M3) |
| test_line_hf | 5.7 | planned (M3) |
| test_qp | 7 | planned (M4) |
| test_wavelet | 8 | planned (M4) |
| test_output | 9 | planned (M3) |
| test_partial | 10 | planned (M5) |
| crxcheck on the corpora | all | running from M3 |
