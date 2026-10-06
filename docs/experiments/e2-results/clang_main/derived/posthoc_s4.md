# POST HOC static baselines that include N (not preregistered)

- S4: per (dtype,k,N): cells 324, median 1.000, p95 1.005, max 1.097, share>1.05 0.006
  - arena_warm: p95 1.026, max 1.065, share>1.05 0.009
  - arena_cold: p95 1.010, max 1.097, share>1.05 0.009
  - glibc_default_fresh: p95 1.000, max 1.002, share>1.05 0.000
- S5: per (dtype,k,N-class small<=16Ki / mid 64Ki-1Mi / large>=4Mi): cells 324, median 1.000, p95 1.050, max 1.115, share>1.05 0.052
  - arena_warm: p95 1.058, max 1.103, share>1.05 0.056
  - arena_cold: p95 1.016, max 1.097, share>1.05 0.028
  - glibc_default_fresh: p95 1.059, max 1.115, share>1.05 0.074
