# Benchmarks

Machine: Kamen's MacBook Pro, M5 Pro, 18 cores. `crxbench -n 3`, decode
from memory, median over files. Reference: LibRaw 0.22.2 unpack of the
same R6 Mark II files, 4 OpenMP threads: 43 ms.

| date | commit | build | R6 II 24 MP, level 0 | level 1 | level 2 | EOS R 30 MP two tiles, level 0 | level 1 | notes |
|---|---|---|---|---|---|---|---|---|
| 2026-09-09 | M4/M5 | 1 thread, no optimisation, all bands decoded at every level | 209 ms | 140 ms | 124 ms | 228 ms | 146 ms | first correct version |
| 2026-09-09 | M6 step 1 | 1 thread; row-oriented wavelet, fused dequant, band skipping | 213 ms | 58 ms | 18 ms | | | measured under load |
| 2026-09-09 | M6 step 2 | 16 threads; bands, stage rows/columns and emit in parallel; shared pool, workspace cache | 19.4 ms | 6.4 ms | 3.5 ms | 18.7 ms | | idle machine; 1 thread full: 164 ms |

Phase breakdown, R6 Mark II, 16 threads, full decode: setup and QP map
1.6 ms, band decoding 10.4 ms (bounded by the largest single band, about
7 ms of serial entropy decoding), three synthesis stages 5.8 ms, emit
1.7 ms.
