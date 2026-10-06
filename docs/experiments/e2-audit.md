# E2 post hoc adversarial audit

Everything in this document is **POST HOC**. The preregistered E2 verdict (`e2-report.md`: REGIME-SENSITIVE PLANNING SURVIVES by gate C only; gates A and B fail) is not changed by it. No new E2 primary data were collected: every number is re-derived from the committed raw samples (`e2-results/{gcc,clang}_main/raw_*.csv`) by `experiments/e2-materialize-vs-recompute/e2_audit.py`; full tables are in `e2-results/audit/audit_{gcc,clang}.md`. The audit script first reproduces the preregistered numbers exactly (S3 p95 1.061 / 1.077, corroborated opportunity cells 20 / 22, gate C pairs 80 / 127 with the 36+43+1 and 50+76+1 kind split), then adds the new analyses.

Audited head: `ab04053f2c37ac1dd9d5cbde592ba93601892c9d` (E2 branch), base `769f27db9bf397c6863e9efcd8e03de0fb7156ff`, preregistration `187e0df266c102c4047b2506270d343cd58af2cd` (ancestor of the head). CI on the head was green (Ubuntu, macOS, Windows, ASan+UBSan).

## A1 - accounting-collision sensitivity

Affected cells (OBSERVED in `strategies_*.csv`, `bookkeeping_collision = 1`): exactly **N = 64 with k = 6 and k = 8**, both dtypes, all three regimes = **12 cells** (of 324). k = 5 and 7 are not in the matrix; k = 1..4 do not collide (capacities 1, 2, 4 give 32, 64, 128 B, not 256 B).

It is **not** only counted. In `arena_warm` and `arena_cold` the hook routes any armed request of exactly the result size through the workspace (`allocate` -> `ws->allocate`), so the executor's 256-byte `outputs` vector block is served by the arena there (counted in `arena_allocs`), and by `malloc` in `glibc_default_fresh`. It is the same in M, R and M_TWIN (the harness checks `allocs == fresh + copies + collision` in every cell), so it cannot bias M versus R within a cell; it can only make these 12 cells differ from their neighbours in how one small block is allocated. The hook never mixes up any other allocation: the exact-count check passed in all other cells, which excludes any further size collision in the matrix.

| 312 cells (12 excluded) | GCC full 324 | GCC excl. | Clang full 324 | Clang excl. |
| --- | --- | --- | --- | --- |
| S3 median / p95 / max | 1.000 / 1.061 / 1.118 | 1.000 / 1.065 / 1.118 | 1.000 / 1.077 / 1.138 | 1.000 / 1.077 / 1.138 |
| S3 share > 1.05 | 0.065 | 0.067 | 0.071 | 0.074 |
| corroborated opportunity cells (rate) | 20 (6.2 %) | 20 (6.4 %) | 22 (6.8 %) | 22 (7.1 %) |
| original gate C pairs / (dtype,k) groups | 80 / 6 | 80 / 6 | 127 / 6 | 127 / 6 |
| S4 p95 / max | 1.006 / 1.117 | 1.007 / 1.117 | 1.005 / 1.097 | 1.005 / 1.097 |

Result: excluding the collision cells changes no gate, no pair count and no regret statistic materially. **No artifact from the collision.**

## A2 - stricter gate C variants (POST HOC; the preregistered gate C is unchanged)

C_STRICT: cells A, B of the same (dtype, k) with opposite decisive winners, **each** winner beating the alternative by >= 5 % in its own cell, both decisive winners reproducing in all three replicates. C_ADJACENT: C_STRICT restricted to adjacent tested N in the same regime, or the same N in different regimes. Semantic groups collapse the pairs of one non-monotone curve to its boundaries.

| | GCC | Clang |
| --- | --- | --- |
| original gate C: pairs / (dtype,k) groups | 80 / 6 | 127 / 6 |
| C_STRICT: pairs / (dtype,k) groups | 75 / 6 | 108 / 5 |
| C_STRICT by kind | N-only 36, mixed 38, regime-only 1 | N-only 42, mixed 66, regime-only 0 |
| C_ADJACENT: pairs / (dtype,k) groups / semantic groups | 7 / 3 / 7 | 10 / 5 / 10 |
| C_ADJACENT by kind | N-only 6, regime-only 1 | N-only 10 |

Meaningful (dtype,k) groups with a flip under C_ADJACENT: GCC f32 k=3, i32 k=3, i32 k=4; Clang f32 k=2, f32 k=3, i32 k=1, i32 k=2, i32 k=3. The 80 and 127 headline counts are inflated by combinatorics: one non-monotone curve produces many pairs. The stable core is **7 (GCC) / 10 (Clang) adjacent boundaries in 3 / 5 (dtype,k) groups**, almost all N boundaries in one regime. Gate C still holds under both stricter variants, on both compilers, but on a much smaller set than "80 / 127 flips" suggests. The single pure-regime pair that survives C_STRICT (GCC only) is i32, k=4, N=64 Ki: `arena_warm` favours R by 9 %, `glibc_default_fresh` favours M by 24 %.

## A3 - size-aware static baselines

S4 = one fixed action per (dtype, k, N), constant across regimes, chosen with all three regimes (oracle-static, in-sample). S4-CV = for each cell, the action is chosen from the *other two* regimes and tested on the held-out one, so the target regime never selects its own action.

| | GCC | Clang |
| --- | --- | --- |
| S4 median / p95 / max / share > 1.05 | 1.000 / 1.006 / 1.117 / 0.006 | 1.000 / 1.005 / 1.097 / 0.006 |
| S4 corroborated opportunity cells | 1 | 1 |
| **S4-CV** median / p95 / max / share > 1.05 | 1.000 / 1.031 / 1.263 / 0.031 | 1.000 / 1.023 / 1.297 / 0.025 |
| S4-CV corroborated opportunity cells (rate) | 9 (2.8 %) | 7 (2.2 %) |
| S4-CV held-out `arena_warm`: p95 / max | 1.031 / 1.091 | 1.026 / 1.065 |
| S4-CV held-out `arena_cold`: p95 / max | 1.019 / 1.117 | 1.011 / 1.097 |
| S4-CV held-out `glibc_default_fresh`: p95 / max / share > 1.05 | 1.119 / 1.263 / 0.065 | 1.050 / 1.297 / 0.056 |

Reading: a table keyed on static facts predicts the best action to within about 3 % at p95 even when the target regime is hidden, so **runtime regime sensitivity is weak in E2 in aggregate** (opportunity rate 2-3 % against the 20 % threshold; p95 1.02-1.03 against 1.15). It is **not zero**: the tail is concentrated in `glibc_default_fresh`, where the choice inferred from the arena regimes is wrong by 21-30 % for k = 4 at N = 64 Ki and 4 Mi, and by about 10-12 % for k = 6 and 8 at N = 64 Ki. Example (GCC, i32, k=4, N=64 Ki): `glibc_default_fresh` M 252 us versus R 313 us (M/R 0.80, decisive), while `arena_warm` favours R decisively (M/R 1.09) and `arena_cold` is a tie (M/R 0.98, not decisive). On Clang the same cell is M/R 0.77 / 1.07 / 1.01. That is a localized effect in 6-9 of 324 cells with corroborated S4-CV regret above 5 %, not a general one, and the arena regimes are a replaced `operator new` pool (not an in-runtime arena), so what it says about a real runtime allocator is unknown.

## A4 - code generation audit

See `e2-codegen-audit.md`. Summary: both compilers vectorize M and R with 128-bit SSE2; GCC 1 vector per iteration, Clang 2; no size-dependent code path other than small-n trip-count guards; the non-monotone N profile is not a code-path artifact; no implementation bias between M and R was found.

## A5 - did E2 remain valid?

- Correctness: 0 mismatches in 87,480 samples (re-checked in the committed CSVs); no arena fallbacks, no hook-table overflows.
- Implementation bias: none found (codegen audit; allocation counts, peak live bytes, moves and copies equal for M and R in every cell).
- Collision cells: no fatal artifact (A1).
- Preregistered result: remains reportable exactly as produced. Interpretation after the audit: the *preregistered* gate C holds, the *stricter* variants also hold but on 7 / 10 semantic boundaries; the flips are driven by N, which is static; regime information beyond (dtype, k, N) is weak in aggregate with a localized tail in the default-allocator regime.
