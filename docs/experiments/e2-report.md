# E2 report - Materialize vs Recompute

Preregistration: [`e2-materialize-vs-recompute.md`](e2-materialize-vs-recompute.md) (Part I frozen at `187e0df`). Data and derived tables: [`e2-results/`](e2-results/) (`gcc_main`, `clang_main`). Evidence classes: MEASURED (timed on this machine), CALCULATED (derived from counts or the byte model), INFERRED (explanation, not tested), NOT VERIFIED.

## Verdict

**REGIME-SENSITIVE PLANNING SURVIVES - by the weakest of the three gates, and with a narrower meaning than the label suggests.**

- Gate C (a stable flip for the same dtype and fan-out) holds on both compilers. Gates A and B fail on both. The kill condition is not met only because C holds.
- The best action for a given (dtype, k) is **not** constant across N: a static rule on dtype and fan-out alone loses up to **1.12x** (GCC) / **1.14x** (Clang) in single cells, p95 1.06 / 1.08, in 6-7 % of cells more than 5 %.
- Of the stable flips, **only 1 (of 80 on GCC, 1 of 127 on Clang) differs by memory regime alone**; all others involve a different N. N is a *static* fact in Lume's IR. A post hoc oracle table per (dtype, k, N) that ignores the regime stays within **p95 1.006 / 1.005** of the optimum. So E2 supports "the choice depends on shape-dependent, machine-specific facts", not "the choice needs runtime or allocator-state information".
- All of this is one KVM guest, one program family, two compilers. Nothing here is established beyond this machine.

## Setup (what was run)

- Program D = A + B, Rj = D + Cj for k in {1,2,3,4,6,8}; f32 and i32; N from 64 to 16 Mi; regimes arena_warm, arena_cold, glibc_default_fresh (one process per cell). 324 cells per compiler.
- Strategy M: policy {reuse, no fusion}; strategy R: the narrow recompute seam (`ExecutionPolicy::recompute`), each consumer runs `(A + B) + C` in one loop into its own fresh buffer. Allocation fairness (MEASURED, 0 differing cells): M and R have identical fresh allocations (k), output moves (k), copies (0), result allocations and peak live result bytes. M runs k+1 loops, R runs k.
- 3 replicates x 15 repetitions x 3 series (M, R, M_TWIN) = 45 samples per series per cell; **43,740 samples per compiler**; all 87,480 correct against the independent oracle; no arena fallbacks, no hook overflows, swap 0, arena_warm minor faults 0.00 per call (median).
- Environment: Intel Xeon @ 2.10 GHz KVM guest, Linux 6.18.44, glibc 2.39, 16 GiB RAM, no PMU, THP madvise; GCC 13.3 / Clang 18.1, `-O3 -DNDEBUG`, no fast-math, no `-march=native`.

## Results (MEASURED)

| policy (regret = median(choice) / median(best)) | GCC median / p95 / max / >1.05 | Clang median / p95 / max / >1.05 |
| --- | --- | --- |
| S1a byte rule, k=3 -> R | 1.000 / 1.107 / 1.293 / 12.7 % | 1.000 / 1.095 / 1.188 / 12.3 % |
| S1b byte rule, k=3 -> M | 1.000 / 1.065 / 1.185 / 7.4 % | 1.000 / 1.077 / 1.138 / 7.1 % |
| S2 best global threshold (R iff k <= T) | T = 1: 1.000 / 1.056 / 1.118 / 6.2 % | T = 2: 1.000 / 1.077 / 1.138 / 7.1 % |
| **S3 best static lookup per (dtype, k)** | **1.000 / 1.061 / 1.118 / 6.5 %** | **1.000 / 1.077 / 1.138 / 7.1 %** |
| always M | 1.000 / 1.198 / 1.412 / 17.3 % | 1.000 / 1.253 / 1.335 / 18.5 % |
| always R | 1.037 / 1.293 / 1.489 / 43.8 % | 1.032 / 1.249 / 1.373 / 42.6 % |

- S3 lookups: GCC f32 k=1 R, k>=2 M; i32 k<=2 R, k>=3 M. Clang: R for k<=2, M for k>=3 in both dtypes.
- Gate A: 20 (GCC) / 22 (Clang) of 324 cells are noise-corroborated S3 opportunities (6.2 % / 6.8 %), threshold 20 %. Gate B: p95 1.061 / 1.077, threshold 1.15. Gate C: 80 / 127 stable flips. Excluding k=1: A 5.6 % / 6.7 %, B 1.065 / 1.073, C still holds (63 / 67 flips).
- Noise floor (M versus its own twin series): median 1.005, p95 1.026 (GCC) / 1.025 (Clang), max 1.12 (GCC, arena_cold) / 1.24 (Clang, glibc_default_fresh). The noise p95 is below the 5 % effect threshold; the maxima are not, which is why decisive winners require >= 3 % and 75 % of units.
- Compilers agree: same winner in 291 / 324 cells; in the 190 cells decisive in both, 189 agree and 1 conflicts. Median absolute log difference of the M/R ratio is 1.6 %.
- Best global threshold T is 1 (GCC) or 2 (Clang): a *global k-threshold exists but is compiler-dependent*, so even the static rule is not portable across the two toolchains.

### Pattern of M/R (GCC, f32, arena_warm; > 1 = R faster)

| k \ N | 64 | 1Ki | 4Ki | 16Ki | 64Ki | 256Ki | 1Mi | 4Mi | 16Mi |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.92 | 1.00 | 1.08 | 1.17 | 1.17 | 1.17 | 1.30 | 1.27 | 1.31 |
| 2 | 0.94 | 0.99 | 0.84 | 0.91 | 0.96 | 0.97 | 1.01 | 1.12 | 1.08 |
| 4 | 0.90 | 0.96 | 0.73 | 0.77 | 1.04 | 0.85 | 0.89 | 1.03 | 0.93 |
| 8 | 0.87 | 0.93 | 0.67 | 0.71 | 1.04 | 0.79 | 0.87 | 1.00 | 0.86 |

Full tables for both compilers, dtypes and regimes are in `e2-results/*/derived/e2_tables.md`.

### k* (secondary)

k* is not constant across N: GCC f32 arena_warm gives k* = all (N=64), 3, 2, 2, **none** (64Ki), 3, 3, **none** (4Mi), 4. At 64Ki and 4Mi (256 KiB and 16 MiB per f32 buffer) R is not clearly beaten even at k=8 in the warm regime, while the neighbouring sizes favour M from k=2-3. i32 is similar. The profile is **non-monotone in N**, which a single threshold cannot represent and which I read as a size-specific effect of this machine's memory hierarchy (INFERRED; not tested: no PMU, no cache-controlled variant).

### Byte model (CALCULATED prediction versus MEASURED winner, k != 3)

Median-winner agreement: GCC 77 % (warm) / 82 % (cold) / 84 % (fresh); Clang 82 / 86 / 89 %. Where a winner is decisive: GCC 62 of 73 / 43 of 45 / 62 of 67; Clang 64 of 72 / 38 of 41 / 66 of 72.

## What the byte model got right and wrong

Right: k=1 prefers R at larger N, increasingly (M/R 1.17-1.31 in the warm regime for N >= 16Ki); large k prefers M at most mid/large N; k=2 prefers R at N >= 4Mi; the k=3 tie is a tie within +-8 % in many cells.

Wrong:
1. At small and cache-resident N (64 .. 16Ki) **M wins even for k=1..3** (k=2, N=4Ki-16Ki: M faster by 9-16 %; k=1, N=64: M faster by 8 %). The byte model predicts the opposite. The preregistered prediction 2 ("R preferred up to larger k at cache-resident sizes") was **wrong in direction**. A likely cause is that the extra loop of M costs little when everything is in cache while the fused loop is less efficient per element (INFERRED; I did not inspect generated code).
2. The 64Ki and 4Mi bumps where R catches up for all k.
3. The cold regime does not shift toward R (prediction 4); it compresses ratios toward 1 (first-touch cost is equal for both strategies).
4. dtype matters at k=1 (i32 M/R 1.35-1.41 at N = 4Ki-64Ki versus f32 1.07-1.17), contrary to prediction 5.

## What E2 shows and does not show

- **Shown (MEASURED, this VM):** the fan-out rule alone is not enough: the best action for the same (dtype, k) flips with N by 5-30 %, reproducibly in all three replicates and on both compilers.
- **Shown:** the memory regime (arena versus default glibc, warm versus cold) moves the winner much less than N does (1 pure-regime flip per compiler), and a table keyed on static facts per machine stays within 1 % (p95) of the best in this data (post hoc S4, an oracle table: it is fitted to the same data and says nothing about generalization to unseen N or machines).
- **Not shown:** that any planner beats a simple per-machine calibrated table; that the effect exists on physical hardware or on ARM64; that it exists beyond additions. The magnitude is modest: S3 loses at most 12-14 %, in about 6.5 % of cells.
- **Not verified:** the cause of the non-monotone N profile (no PMU; no cache-size-controlled experiment; generated code not inspected); the VM's cache topology.

## Threats to validity

1. **Single KVM guest.** Virtualized cache topology and neighbours; the non-monotone profile may be a property of this guest. Strongest threat: a positive result here needs physical x86 and ARM64 replication before any architecture work.
2. M and R are different loops: the difference includes compiler vectorization and unrolling of two different kernels, not only memory traffic. Two compilers agree (not independent of the kernel shape).
3. Oracle-static baselines (S2, S3, S4, S5) are fitted with the data and are optimistic by construction.
4. Gate C counts pairs in the whole N x regime grid; most qualifying flips are size flips with N known statically.
5. The arena is a replaced `operator new` pool, not an in-runtime arena; alignment (64 B versus 16 B) differs by regime, not by strategy.
6. Harness-debug pilot (discarded, nothing changed afterwards) and the post hoc S4/S5 analyses are disclosed in Part II.

## Recommendation

E2 passes the preregistered survive criterion, but the evidence points to **per-machine calibration keyed on static facts (dtype, fan-out, N), not to a runtime, regime-sensitive planner**. The recommended next action is therefore **physical-machine replication (x86 and ARM64) of this exact matrix, before any architecture work**; `e2-next-architecture.md` records the facts that would feed a planner if replication confirms the effect. Do not build the planner, `CostModel`, or `verify_plan` until then. If replication shows the size-dependent flips disappear or the best choice follows a monotone rule, the planner thesis is dead and the options become A (Experiment 003, encodings) or B (reposition Lume).
