# Benchmarks

Machine: Kamen's MacBook Pro, M5 Pro, 18 cores. `crxbench -n 3`, decode
from memory, median over files. Reference: LibRaw 0.22.2 unpack of the
same R6 Mark II files, 4 OpenMP threads: 43 ms.

| date | commit | build | R6 II 24 MP, level 0 | level 1 | level 2 | EOS R 30 MP two tiles, level 0 | level 1 | notes |
|---|---|---|---|---|---|---|---|---|
| 2026-09-09 | M4/M5 | 1 thread, no optimisation, all bands decoded at every level | 209 ms | 140 ms | 124 ms | 228 ms | 146 ms | first correct version |
| 2026-09-09 | M6 step 1 | 1 thread; row-oriented wavelet, fused dequant, band skipping | 213 ms | 58 ms | 18 ms | | | measured under load |
| 2026-09-09 | M6 step 2 | 16 threads; bands, stage rows/columns and emit in parallel; shared pool, workspace cache | 19.4 ms | 6.4 ms | 3.5 ms | 18.7 ms | | idle machine; 1 thread full: 164 ms |

| 2026-09-09 | M6 step 3 | 16 threads; strip synthesis with fused emit, four-byte refill | 16.1 ms | 6.0 ms | | 16.6 ms | | best of several runs; see the note on noise |

Phase breakdown after step 3, R6 Mark II, 16 threads, full decode: setup
and QP map 1.6 ms, band decoding 10.0 ms (bounded by the largest single
band, about 7 ms of serial entropy decoding), synthesis and emit 3.1 ms.
Peak RSS of crxbench 211 MB, of which the file is 24 MB and the output
51 MB: about 136 MB of decoder workspace, 2.7x the output.

Noise. After hours of sustained load (corpus runs, three fuzzers) the
same binary measured 16 ms and 28 ms for the same files minutes apart;
the machine throttles. Comparisons must interleave A and B and take the
minimum over repetitions, on a cool machine. crxbench should grow a
`--min` mode before the next optimisation round.
