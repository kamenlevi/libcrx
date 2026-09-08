# The CRX format, as libcrx understands it

Every formula the decoder uses lives here first. A section is complete when
it has: the rule, its source, a hand-worked example small enough to check on
paper, and the name of the unit test that encodes that example. Code with no
section here does not merge.

Sources, in order of authority:

1. Behaviour of real files from the corpus, observed with our own tools.
2. Laurent Clévy, *canon_cr3* (github.com/lclevy/canon_cr3), the public
   description of the container and the CRX codec.
3. Reference decoders (LibRaw, rawspeed) as black boxes: inputs in, sensor
   values out. Their source is consulted to understand behaviour, never
   copied.

Notation: all values are integers. `>>` is an arithmetic shift on signed
values. Bit widths are written as `s16`, `u14` and so on.

## 1. Container (ISO BMFF)

*Milestone 1.* The boxes on the path to the image: `ftyp`, `moov` with the
Canon `uuid` box, four `trak` boxes and their `stsd`/`CRAW` entries,
`stsz`/`co64` giving the size and offset of each track's data, and which
track holds the full-size raw.

## 2. CRX headers

*Milestone 1.* The `CMP1` box (image dimensions, bit depth, plane count,
wavelet levels, tile geometry, encoding flags) and the tile, plane and
subband headers at the start of the codestream.

## 3. Entropy coding

*Milestone 1, tested in 3.* Adaptive Golomb-Rice with run mode. The
parameter adaptation rule, the run-length rule and its escape, the mapping
from unsigned codes to signed residuals, and the prediction from the row
above.

## 4. Lossless reconstruction

*Milestone 1, tested in 3.* From residuals to sensor values for a plane
with zero wavelet levels; how the four planes interleave into the Bayer
mosaic; the bit depth and any offset.

## 5. Wavelet

*Milestone 1, tested in 4.* The integer 5/3 lifting transform used by
C-RAW: analysis and synthesis, boundary extension, and the bound on
coefficient growth per level (which fixes the intermediate bit widths).

## 6. Quantisation

*Milestone 1, tested in 4.* How subband coefficients are dequantised in
lossy mode; the per-subband parameters and where they are read from.

## 7. Partial decode

*Milestone 5.* Which subbands a level-N output needs, and the proof that
stopping synthesis after N levels equals the LL band of the analysis of the
full image (this is the definition of `crx_decode(level > 0)`).

## 8. Bit widths and overflow

*Every milestone.* One table: each intermediate quantity, its range, its
storage type, and the reason the range holds.
