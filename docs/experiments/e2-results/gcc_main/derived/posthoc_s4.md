# POST HOC static baselines that include N (not preregistered)

- S4: per (dtype,k,N): cells 324, median 1.000, p95 1.006, max 1.117, share>1.05 0.006
  - arena_warm: p95 1.010, max 1.091, share>1.05 0.009
  - arena_cold: p95 1.019, max 1.117, share>1.05 0.009
  - glibc_default_fresh: p95 1.000, max 1.012, share>1.05 0.000
- S5: per (dtype,k,N-class small<=16Ki / mid 64Ki-1Mi / large>=4Mi): cells 324, median 1.000, p95 1.038, max 1.118, share>1.05 0.043
  - arena_warm: p95 1.055, max 1.092, share>1.05 0.056
  - arena_cold: p95 1.027, max 1.117, share>1.05 0.019
  - glibc_default_fresh: p95 1.054, max 1.118, share>1.05 0.056
