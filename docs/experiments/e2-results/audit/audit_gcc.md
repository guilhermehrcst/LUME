## gcc (POST HOC audit of committed raw data)

Excluded accounting-collision cells: 12 (N = 64, k in {6, 8}, both dtypes, all three regimes).

### All 324 cells (must equal the preregistered analysis)

| quantity | value |
| --- | --- |
| cells | 324 |
| S3 median / p95 / max / >1.05 | 1.000 / 1.061 / 1.118 / 0.065 |
| S3 corroborated opportunity cells (rate) | 20 (0.062) |
| original Gate C: stable pairs / unique (dtype,k) groups | 80 / 6 |
| original Gate C by kind | {'N-only': 36, 'mixed': 43, 'regime-only': 1} |
| C_STRICT: pairs / (dtype,k) groups | 75 / 6 |
| C_STRICT by kind | {'N-only': 36, 'mixed': 38, 'regime-only': 1} |
| C_ADJACENT: pairs / (dtype,k) groups / semantic groups | 7 / 3 / 7 |
| C_ADJACENT by kind | {'N-only': 6, 'regime-only': 1} |
| S4 median / p95 / max / >1.05 | 1.000 / 1.006 / 1.117 / 0.006 |
| S4 corroborated opportunity cells | 1 |
| S4 p95 / max by regime | arena_warm: 1.010 / 1.091; arena_cold: 1.019 / 1.117; glibc_default_fresh: 1.000 / 1.012 |
| S4-CV median / p95 / max / >1.05 | 1.000 / 1.031 / 1.263 / 0.031 |
| S4-CV corroborated opportunity cells (rate) | 9 (0.028) |
| S4-CV p95 / max by held-out regime | arena_warm: 1.031 / 1.091 / >1.05 0.019; arena_cold: 1.019 / 1.117 / >1.05 0.009; glibc_default_fresh: 1.119 / 1.263 / >1.05 0.065 |

### Excluding the collision cells

| quantity | value |
| --- | --- |
| cells | 312 |
| S3 median / p95 / max / >1.05 | 1.000 / 1.065 / 1.118 / 0.067 |
| S3 corroborated opportunity cells (rate) | 20 (0.064) |
| original Gate C: stable pairs / unique (dtype,k) groups | 80 / 6 |
| original Gate C by kind | {'N-only': 36, 'mixed': 43, 'regime-only': 1} |
| C_STRICT: pairs / (dtype,k) groups | 75 / 6 |
| C_STRICT by kind | {'N-only': 36, 'mixed': 38, 'regime-only': 1} |
| C_ADJACENT: pairs / (dtype,k) groups / semantic groups | 7 / 3 / 7 |
| C_ADJACENT by kind | {'N-only': 6, 'regime-only': 1} |
| S4 median / p95 / max / >1.05 | 1.000 / 1.007 / 1.117 / 0.006 |
| S4 corroborated opportunity cells | 1 |
| S4 p95 / max by regime | arena_warm: 1.010 / 1.091; arena_cold: 1.019 / 1.117; glibc_default_fresh: 1.000 / 1.012 |
| S4-CV median / p95 / max / >1.05 | 1.000 / 1.035 / 1.263 / 0.032 |
| S4-CV corroborated opportunity cells (rate) | 9 (0.029) |
| S4-CV p95 / max by held-out regime | arena_warm: 1.031 / 1.091 / >1.05 0.019; arena_cold: 1.019 / 1.117 / >1.05 0.010; glibc_default_fresh: 1.119 / 1.263 / >1.05 0.067 |

C_ADJACENT semantic groups (all cells), `(dtype, k, regime, 'N-boundary', lower N)` or `(dtype, k, N, 'regime', pair)`:

- ('f32', 3, 'arena_warm', 'N-boundary', 16384)
- ('f32', 3, 'arena_warm', 'N-boundary', 65536)
- ('f32', 3, 'glibc_default_fresh', 'N-boundary', 1048576)
- ('i32', 3, 'arena_warm', 'N-boundary', 65536)
- ('i32', 4, 'arena_warm', 'N-boundary', 16384)
- ('i32', 4, 'arena_warm', 'N-boundary', 65536)
- ('i32', 4, 65536, 'regime', ('arena_warm', 'glibc_default_fresh'))

S4-CV corroborated opportunity cells (all 324 cells; chosen without the cell's own regime):

| dtype | k | N | held-out regime | CV action | M ns | R ns | regret |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: |
| f32 | 3 | 65536 | arena_warm | M | 57591 | 54600 | 1.055 |
| f32 | 4 | 65536 | glibc_default_fresh | R | 241857 | 305360 | 1.263 |
| f32 | 4 | 4194304 | glibc_default_fresh | R | 31798075 | 38515378 | 1.211 |
| f32 | 6 | 65536 | glibc_default_fresh | M | 142291 | 129433 | 1.099 |
| f32 | 8 | 65536 | glibc_default_fresh | R | 622090 | 699049 | 1.124 |
| i32 | 4 | 65536 | arena_warm | M | 82655 | 75783 | 1.091 |
| i32 | 4 | 65536 | glibc_default_fresh | R | 251547 | 312836 | 1.244 |
| i32 | 4 | 4194304 | glibc_default_fresh | R | 31731304 | 39126202 | 1.233 |
| i32 | 8 | 65536 | glibc_default_fresh | R | 621259 | 695399 | 1.119 |
