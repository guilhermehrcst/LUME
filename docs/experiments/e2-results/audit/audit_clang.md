## clang (POST HOC audit of committed raw data)

Excluded accounting-collision cells: 12 (N = 64, k in {6, 8}, both dtypes, all three regimes).

### All 324 cells (must equal the preregistered analysis)

| quantity | value |
| --- | --- |
| cells | 324 |
| S3 median / p95 / max / >1.05 | 1.000 / 1.077 / 1.138 / 0.071 |
| S3 corroborated opportunity cells (rate) | 22 (0.068) |
| original Gate C: stable pairs / unique (dtype,k) groups | 127 / 6 |
| original Gate C by kind | {'N-only': 50, 'mixed': 76, 'regime-only': 1} |
| C_STRICT: pairs / (dtype,k) groups | 108 / 5 |
| C_STRICT by kind | {'N-only': 42, 'mixed': 66} |
| C_ADJACENT: pairs / (dtype,k) groups / semantic groups | 10 / 5 / 10 |
| C_ADJACENT by kind | {'N-only': 10} |
| S4 median / p95 / max / >1.05 | 1.000 / 1.005 / 1.097 / 0.006 |
| S4 corroborated opportunity cells | 1 |
| S4 p95 / max by regime | arena_warm: 1.026 / 1.065; arena_cold: 1.010 / 1.097; glibc_default_fresh: 1.000 / 1.002 |
| S4-CV median / p95 / max / >1.05 | 1.000 / 1.023 / 1.297 / 0.025 |
| S4-CV corroborated opportunity cells (rate) | 7 (0.022) |
| S4-CV p95 / max by held-out regime | arena_warm: 1.026 / 1.065 / >1.05 0.009; arena_cold: 1.011 / 1.097 / >1.05 0.009; glibc_default_fresh: 1.050 / 1.297 / >1.05 0.056 |

### Excluding the collision cells

| quantity | value |
| --- | --- |
| cells | 312 |
| S3 median / p95 / max / >1.05 | 1.000 / 1.077 / 1.138 / 0.074 |
| S3 corroborated opportunity cells (rate) | 22 (0.071) |
| original Gate C: stable pairs / unique (dtype,k) groups | 127 / 6 |
| original Gate C by kind | {'N-only': 50, 'mixed': 76, 'regime-only': 1} |
| C_STRICT: pairs / (dtype,k) groups | 108 / 5 |
| C_STRICT by kind | {'N-only': 42, 'mixed': 66} |
| C_ADJACENT: pairs / (dtype,k) groups / semantic groups | 10 / 5 / 10 |
| C_ADJACENT by kind | {'N-only': 10} |
| S4 median / p95 / max / >1.05 | 1.000 / 1.005 / 1.097 / 0.006 |
| S4 corroborated opportunity cells | 1 |
| S4 p95 / max by regime | arena_warm: 1.026 / 1.065; arena_cold: 1.010 / 1.097; glibc_default_fresh: 1.000 / 1.002 |
| S4-CV median / p95 / max / >1.05 | 1.000 / 1.026 / 1.297 / 0.026 |
| S4-CV corroborated opportunity cells (rate) | 7 (0.022) |
| S4-CV p95 / max by held-out regime | arena_warm: 1.026 / 1.065 / >1.05 0.010; arena_cold: 1.011 / 1.097 / >1.05 0.010; glibc_default_fresh: 1.050 / 1.297 / >1.05 0.058 |

C_ADJACENT semantic groups (all cells), `(dtype, k, regime, 'N-boundary', lower N)` or `(dtype, k, N, 'regime', pair)`:

- ('f32', 2, 'glibc_default_fresh', 'N-boundary', 1024)
- ('f32', 2, 'glibc_default_fresh', 'N-boundary', 64)
- ('f32', 3, 'glibc_default_fresh', 'N-boundary', 65536)
- ('i32', 1, 'arena_warm', 'N-boundary', 64)
- ('i32', 2, 'arena_warm', 'N-boundary', 1024)
- ('i32', 2, 'arena_warm', 'N-boundary', 64)
- ('i32', 2, 'glibc_default_fresh', 'N-boundary', 1024)
- ('i32', 2, 'glibc_default_fresh', 'N-boundary', 64)
- ('i32', 3, 'glibc_default_fresh', 'N-boundary', 16384)
- ('i32', 3, 'glibc_default_fresh', 'N-boundary', 65536)

S4-CV corroborated opportunity cells (all 324 cells; chosen without the cell's own regime):

| dtype | k | N | held-out regime | CV action | M ns | R ns | regret |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: |
| f32 | 3 | 1024 | glibc_default_fresh | R | 5044 | 5312 | 1.053 |
| f32 | 4 | 65536 | glibc_default_fresh | R | 272864 | 340808 | 1.249 |
| f32 | 4 | 4194304 | glibc_default_fresh | R | 31960228 | 39361008 | 1.232 |
| i32 | 2 | 262144 | glibc_default_fresh | R | 298747 | 313761 | 1.050 |
| i32 | 4 | 65536 | arena_warm | M | 82810 | 77737 | 1.065 |
| i32 | 4 | 65536 | glibc_default_fresh | R | 255682 | 331501 | 1.297 |
| i32 | 8 | 65536 | glibc_default_fresh | R | 626846 | 705428 | 1.125 |
