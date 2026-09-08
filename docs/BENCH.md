# Benchmarks

Machine: Kamen's MacBook Pro, M5 Pro, 18 cores. `crxbench -n 3`, decode
from memory, median over files. Reference: LibRaw 0.22.2 unpack of the
same R6 Mark II files, 4 OpenMP threads: 43 ms.

| date | commit | build | R6 II 24 MP, level 0 | level 1 | level 2 | EOS R 30 MP two tiles, level 0 | level 1 | notes |
|---|---|---|---|---|---|---|---|---|
| 2026-09-09 | M4/M5 | 1 thread, no optimisation, all bands decoded at every level | 209 ms | 140 ms | 124 ms | 228 ms | 146 ms | first correct version |
