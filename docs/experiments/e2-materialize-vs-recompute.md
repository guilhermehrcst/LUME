# E2 - Materialize vs Recompute

> **Status: PREREGISTERED. No E2 performance data had been collected or inspected when this file was committed.**
> Part I (this part) is frozen at that commit. Results go into Part II or `e2-report.md`; any change to Part I would be a separate, labelled amendment commit.

## Part I - Preregistration

### 1. Scientific question

When a shared intermediate has several consumers, does the best choice between

1. **materializing** it once, and
2. **recomputing** it independently inside every consumer

depend enough on size, memory regime or machine state that a static rule on semantic facts (dtype, fan-out) is no longer near-optimal?

E1 ([report](e1-report.md)) found no decision in the P5/P6/P7 space because that space is ordered by construction. E2 is the first Lume experiment whose two strategies do **not** dominate each other by construction: a plain byte model predicts a crossover (§4), so finding *a* crossover is not the question. The question is whether the crossover **moves**.

E2 does not build a planner, cost model, plan verifier, new opcode or recomputation framework.

### 2. Verified starting facts

- `main` = `769f27db9bf397c6863e9efcd8e03de0fb7156ff`, the merge commit of PR #13 (E1); `ded583b` (E1 preregistration) is an ancestor.
- Available and reused from E1: `ExecutionPolicy`/`ExecutionStats` seam (`src/runtime/execution_policy.hpp`), workspace and allocation hook (`experiments/e1-is-planning-necessary/{workspace.hpp,alloc_hooks.*}`), the oracle-checked harness pattern, `analyze.py` conventions, twin noise control, fresh-process regime.
- i32 add wraps modulo 2^32 (defined); the oracle (`lume_oracle`) does not link the runtime; the verifier rejects zero-length types, so N = 0 is not legal.
- `FLT_EVAL_METHOD == 0` on the measurement toolchains. The M7 argument that a loop-local `a[i] + b[i]` equals the stored binary32 intermediate applies unchanged; where it cannot be guaranteed, f32 recompute is not run and the cell is reported as skipped.

### 3. Program family and strategies

Only `input`, `add`, `output`. For fan-out k:

```text
D  = A + B
R1 = D + C1 ... Rk = D + Ck        (each consumer: D is the left operand)
output R1 ... Rk                    (D is not an output)
```

Inputs are A, B, C1..Ck (k + 2 caller buffers, all length N); outputs R1..Rk.

**Strategy M (materialize)** = executor policy `{reuse: on, fuse: off, recompute: off}`: compute D once into a fresh buffer, then every consumer from D; the **last** consumer legally reuses D's storage (M6). Nothing is handicapped: all current legal reuse is used. (M7 fusion is not applicable for k ≥ 2 because D has several consumers; for k = 1 it is switched off so that M really materializes.)

**Strategy R (recompute)** = executor policy `{reuse: on, fuse: off, recompute: on}`: D is never stored; each consumer j runs the fused loop `t = A[i] + B[i]; R_j[i] = t + C_j[i]` into its own fresh buffer. Grouping is `(A + B) + C_j`, never reassociated; i32 uses the wrapping helper twice; no fast-math; the loop is the M7 `add_add` kernel.

The recompute mechanism is a deliberately narrow experimental seam, recognizing exactly this pattern (a run of ≥ 1 adjacent adds each using the producer's result once, the producer's result used nowhere else, producer operands and consumer co-operands all caller inputs). It adds no opcode, no IR rewrite and no generic optimizer, and it can be deleted with the experiment.

**Allocation fairness (CALCULATED from the executor's stats, also measured by the allocation hook):** M makes k fresh result buffers (D and R1..R_{k-1}); R_k is computed in D's storage. R makes k fresh result buffers. Fresh allocations, output moves (k) and output copies (0) are therefore **equal**; peak live result bytes are equal (k·4N). What differs: M executes k + 1 loops (one more than R) and moves 12 + 12k bytes/element against 16k (§4). If any measured count differs, the report says so.

### 4. Logical payload model (not measured traffic)

Per element, ignoring caches: M = 12 + 12k; R = 16k.

| k | M | R | model |
| ---: | ---: | ---: | --- |
| 1 | 24 | 16 | R |
| 2 | 36 | 32 | R |
| 3 | 48 | 48 | tie |
| 4 | 60 | 64 | M |
| 6 | 84 | 96 | M |
| 8 | 108 | 128 | M |

This is a model of requested bytes, not DRAM traffic. Cache reuse, prefetching, vectorization, allocation, first-touch and the memory hierarchy can move the crossover, which is what E2 tests. No PMU is available (KVM guest); no claim about DRAM behaviour will be labelled MEASURED.

### 5. Hypotheses

- **H0 - static suffices.** The best choice is adequately determined by static facts, especially fan-out; a runtime/memory-regime-sensitive planner is unnecessary.
- **H1 - regime dependence.** For the same (dtype, fan-out), the best strategy changes meaningfully with N and/or the memory regime, and no static rule on (dtype, fan-out) stays near the empirical optimum.

### 6. Axes (324 cells per compiler)

- **dtype**: f32, i32 (static information; a static policy may differ per dtype).
- **fan-out k**: 1, 2, 3, 4, 6, 8.
- **N (elements)**: 64, 1 Ki, 4 Ki, 16 Ki, 64 Ki, 256 Ki, 1 Mi, 4 Mi, 16 Mi. Regimes are *not* labelled from the VM's reported cache sizes. Largest working set (k = 8, N = 16 Mi): ≈ 1.3 GiB of buffers plus oracle copies, below the 16 GiB RAM; swap must be absent (checked and recorded).
- **memory regime** (three, as in E1, applying its lessons):
  1. `arena_warm`: result-sized allocations served by the reusable workspace with resident pages: no allocator calls and 0 page faults in the timed interval (verified from the fault counter).
  2. `arena_cold`: same workspace, used pages released (`MADV_DONTNEED`) before every timed sample: first-touch faults, no allocator calls.
  3. `glibc_default_fresh`: default glibc allocator, **one process per cell** so that the allocator's dynamic thresholds are not adapted by earlier cells (the E1 finding).
  A steady-allocator regime (`glibc_t1_fresh`) is **not** included, to bound cost; adding it later would be a labelled post-hoc extension.

Cells = 2 × 6 × 9 × 3 = 324. Both strategies are valid in every cell, so every cell is a decision cell. (A sensitivity analysis also reports the gates without k = 1, the control.)

### 7. Measurement protocol

- Sample = one timed interval of K calls (K = 1000 for N < 4096, else 1); results kept until the interval ends and checked against the oracle **outside** the timed region; per-call time = interval / K. Each timed sample is preceded by an untimed priming interval of the same strategy.
- Series per cell: **M**, **R** and **M_TWIN** (a second, independent series of M; noise calibration only, never a strategy). 15 repetitions per series per replicate; series order rotated every repetition (Latin square); **3 independent process replicates**; a *unit* = one (replicate, repetition) with all series adjacent in time. 45 samples per series per cell.
- Statistic: median over pooled samples; spread: IQR; comparisons: paired per-unit ratios; raw samples retained; no outlier removal.
- Recorded per sample: wall ns/call; user/system CPU; minor and major faults; result-sized allocations, bytes, arena fallbacks, table overflows; the executor's own counts (fresh results, in-place results, recomputed consumers, output moves, copies); peak live result bytes (from an untimed dry run); logical payload bytes/element and passes (CALCULATED from the counts: add loop 12 B, fused recompute loop 16 B, output copy 8 B, move 0).
- Environment record: commit, compiler, flags, glibc, kernel, CPU, THP, swap, peak RSS, VM note.
- **Compilers**: GCC 13.3 Release (`-O3 -DNDEBUG`) primary; Clang 18.1 Release as a separately reported replication. No `-ffast-math`, no `-march=native`.
- **Infrastructure change (data independent):** the E1 allocation hook tracks live counted blocks in a hash set sized for 4096 blocks; E2 with K = 1000 and k = 8 holds 8000 results, so the table is enlarged (and the E1 hook test updated). Tracking overflow invalidates a cell.

### 8. Correctness (a failure invalidates the cell and aborts the run)

Every output of every timed strategy is compared exactly (non-NaN bit-exact, NaN matches NaN, i32 exact) with the independent oracle's `(A + B) + C_j`, **not** merely with the other strategy. Tests (added with the mechanism): k ∈ {1, 2, 3, 4, 8}, f32 and i32, N ∈ {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099}, f32 specials (±0, ±inf, NaNs, subnormals), i32 wrap boundaries, input immutability, every output, executor stats of both strategies, default behaviour unchanged, and a documented mutation of the recompute path that the tests must catch.

### 9. Static baselines (all evaluated against the empirical optimum)

For each cell, regret(policy) = median(time of the policy's choice) / median(time of the empirically best strategy). Medians pooled over the 45 samples.

- **S1 - theoretical byte rule**: k ≤ 2 → R, k ≥ 4 → M. k = 3 is a tie: both fixed tie choices are reported (S1a: R, S1b: M); neither is selected after looking at data.
- **S2 - best global threshold** (post-measurement *oracle-static*): "R iff k ≤ T" for T ∈ {0, 1, 2, 3, 4, 6, 8} (T = 0 is always-M, T = 8 always-R). The best T minimizes the mean of log regret over all cells (ties: smaller T). Also reported per dtype.
- **S3 - best static lookup** (post-measurement *oracle-static*, the primary baseline): one fixed action per (dtype, k), identical for all N and regimes, chosen to minimize the mean log regret over that pair's 27 cells (ties: M). This gives static rules every reasonable advantage; it is not a deployable learner.

### 10. Gates (primary: S3)

Over all 324 cells, pooled replicates:

- **A. Opportunity rate**: ≥ 20 % of cells are *noise-corroborated opportunity cells*: S3's choice a and the other strategy b satisfy median(t_a)/median(t_b) > 1.05 **and** the lower end (2.5th percentile) of a 1000-resample paired bootstrap over units of the median per-unit ratio t_a/t_b exceeds 1.00 (fixed seed).
- **B. Tail regret**: p95 (nearest rank) of S3 regret ≥ 1.15.
- **C. Stable regime-dependent flip**: for the same (dtype, k) there are two cells (different N and/or regime) where M decisively wins in one and R decisively wins in the other, **and** running the first cell's winner in the second costs ≥ 5 % (median ratio ≥ 1.05), **and** the same two decisive winners hold when each of the 3 replicates is analysed alone (15 units each). *Decisive win*: the winner's median is ≥ 3 % lower and it wins in ≥ 75 % of units. Noise-level swaps do not count.

**Survive** iff at least one of A, B, C holds. **Kill condition** (stricter): S3 within 5 % of the optimum in ≥ 95 % of cells **and** p95 < 1.15 **and** no stable flip for any (dtype, k).

### 11. Verdicts

- **REGIME-SENSITIVE PLANNING SURVIVES** iff A, B or C holds.
- **STATIC RULE SUFFICES** iff none holds (reported with whether the kill condition is strictly met).
- **INCONCLUSIVE** only if (i) a correctness failure or invalid cell cannot be resolved, or (ii) the measured noise floor is comparable to the effect thresholds (twin "regret" p95 ≥ 1.10), or (iii) an implementation bias between M and R is found that cannot be removed. Disappointing results are not "inconclusive".

If E2 survives, the report is accompanied by `e2-next-architecture.md` (only facts E2 shows to matter) and a recommendation to replicate on physical hardware before any architecture lands in core. If it fails, the report says plainly what remains valuable in Lume and recommends A (Experiment 003, encodings), B (reposition) or C (stop the planner thesis).

### 12. Secondary result: the crossover k*

For every (dtype, N, regime): k* = smallest tested k such that M wins decisively at k and at every larger tested k; "none" when M is never clearly preferable, "all" when it wins decisively from k = 1. Reported without forcing a crossover. Constant k* across N and regime supports a static rule; k* moving materially supports planning. Also reported: the byte model's prediction versus the empirical winner by cell.

### 13. Predictions made before any E2 measurement (so they can be wrong)

1. In `arena_warm` at N ≥ 1 Mi (beyond the likely cache-resident sizes) the winner follows the byte model: R for k ≤ 2, M for k ≥ 4; k = 3 within noise.
2. At cache-resident sizes (N ≤ 64 Ki) R is preferred up to larger k than the model says, because repeated A/B reads hit cache while M still pays a full extra pass; k* is larger there.
3. Therefore k* moves with N, and a static (dtype, k) lookup shows regret > 1.05 in some cells; confidence moderate, not high.
4. `arena_cold`/`glibc_default_fresh` shift the crossover toward R (first-touch cost is equal for M and R by §3, diluting differences) rather than toward M.
5. dtype does not matter (f32 ≈ i32).
6. The verdict is open. If prediction 3 is wrong the verdict is STATIC RULE SUFFICES.

### 14. Threats known in advance

One KVM guest (no PMU, virtualized cache topology, ±10 % series-level noise seen before); the arena is a replaced `operator new` pool, not an in-runtime arena (64-byte versus 16-byte alignment differs by regime, not by strategy); M executes one more loop than R, so loop-overhead effects at small N are part of the measured difference, not removable; the executor seam adds a detector cost to both strategies equally; allocator effects depend on process history (hence fresh processes); only add/i32/f32 and one program family; oracle-static baselines are optimistic by design; **a positive result would not establish a cross-machine planner thesis** (physical x86 and ARM64 replication would be required), and a negative result is also only for this VM and this space.

### 15. Commit order (verifiable in `git log`)

1. this preregistration; 2. recompute strategy seam; 3. its tests; 4. E2 harness and analysis; 5. primary data (GCC); 6. Clang replication; 7. report. No data before commit 1.

---

## Part II - Results and deviations

Written after the data were collected. Part I above is unchanged from commit `187e0df`. Numbers, tables and interpretation: [`e2-report.md`](e2-report.md); raw data and derived tables: [`e2-results/`](e2-results/).

### Outcome against the preregistered gates (primary baseline S3)

| | GCC 13.3 | Clang 18.1 |
| --- | --- | --- |
| A: corroborated opportunity cells >= 20 % | 20 / 324 = 6.2 % - **no** | 22 / 324 = 6.8 % - **no** |
| B: S3 p95 regret >= 1.15 | 1.061 - **no** | 1.077 - **no** |
| C: stable same-(dtype,k) flip, wrong action >= 5 %, all 3 replicates | 80 flips - **yes** | 127 flips - **yes** |
| kill condition | not met (C holds) | not met (C holds) |

Verdict: **REGIME-SENSITIVE PLANNING SURVIVES, by gate C alone** (see the report for what that does and does not mean).

### Deviations and facts discovered while implementing

1. **Hook table size (anticipated in section 7).** The E1 allocation hook was enlarged from 8192 to 32768 slots (13 to 15 bits) and its overflow test updated. Data independent.
2. **Accounting collision (not anticipated).** At N = 64 and k = 5..8 the executor's `outputs` vector grows to 8 x sizeof(Buffer) = 256 B = 4 x 64, which the exact-size hook counts as one result-sized allocation. It is identical for M and R. The harness fails closed unless the measured allocation count equals `fresh + copies + this known collision`; the collision is recorded in the `bookkeeping_collision` column. Found by the harness's own check on the first smoke run, before any E2 data.
3. **Harness-debug pilot.** After the harness and analysis script ran, one tiny pilot (2 replicates x 5 repetitions x 3 sizes) was run to debug `analyze.py`. Its output was glanced at, was not kept, and nothing in the design, gates or thresholds was changed afterwards.
4. `glibc_t1_fresh` was not run (section 6 said it is not included).
5. **Gate C pair rule.** "Running the first cell's winner in the second costs >= 5 %" was implemented for either ordering of the unordered cell pair (the larger of the two costs must be >= 5 %). Decisive-winner and replicate-stability rules are as written.
6. **k* definition** is the one in section 12 (decisive M at k and every larger tested k).
7. Exact counts: 324 cells, 3 strategies (M, R, M_TWIN), 15 repetitions, 3 replicates: **43,740 samples per compiler, 87,480 in total**, 0 correctness failures, 0 arena fallbacks, 0 hook-table overflows, swap total 0.
8. **Mutation test** (recompute `A + A` instead of `A + B`): 1012 failures in `lume_recompute_test`, reverted.
9. **Post hoc analyses (not preregistered, labelled as such):** S4 = best fixed action per (dtype, k, N) constant across regimes; S5 = per (dtype, k, three N classes) (`posthoc_s4.py`, `derived/posthoc_s4.md`). They were added after seeing that almost all stable flips are driven by N alone, to ask whether the planner thesis needs the *regime* or only static facts.
10. **Post hoc adversarial audit** (collision-cell exclusion, C_STRICT, C_ADJACENT, S4, S4-CV, code generation audit): [`e2-audit.md`](e2-audit.md), [`e2-codegen-audit.md`](e2-codegen-audit.md). It does not change the verdict above.
