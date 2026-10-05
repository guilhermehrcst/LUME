# E1 — Is Planning Necessary?

> **Status: PREREGISTERED. No E1 performance data had been collected or inspected when this file was committed.**
> Part I (this part) is frozen at that commit. Part II (results) is added by later commits and never edits Part I. Any later change to Part I would be a separate, labelled amendment commit with its reason, and the verdict would have to be reported under the original rules as well.

## Part I — Preregistration

### 1. Scientific question

Is there sufficient variation in the empirically optimal execution strategy across workloads, sizes and memory regimes to justify an automatic execution planner (analysis → facts → plan → verified plan → cost model → execute)?

E1 does **not** build a planner, a cost model, a new IR, new operations, encodings or backends. It tests only the prerequisite: whether there is a non-trivial decision to make in the space the current executor can actually express.

### 2. What is not claimed

Last-use reuse, fusion, arena/workspace allocation and allocation/page-fault cost terms are established prior art. E1 makes no novelty claim about any mechanism. A negative result kills only "a sophisticated planner is needed for this tested plan space"; it does not kill Lume.

### 3. Verified starting facts (re-checked against the code at `e046b6f`, not copied from earlier reviews)

- **i32 add is defined, wrapping modulo 2^32.** `wrapping_add` in `src/runtime/cpu_reference.cpp` adds as `uint32_t` and converts back; the oracle's `native_add` for `int32_t` does the same independently.
- **The oracle does not link the runtime.** `oracle/CMakeLists.txt` defines `lume_oracle` as an `INTERFACE` library with include directories only, and its header includes no Lume header.
- Canonical `main` = `e046b6f66a8f3708f205984648149b021a172859` (M7 merged, CI green on that commit, no open PRs, no AGENTS.md).
- Allocation in the executor happens in exactly two places: `add_buffers` (fresh result) and `add_add` (fused result), both `OwnedArray<T>::for_overwrite(n)`; output copies allocate through `OwnedArray`'s deep copy. Executor-owned: every add result; borrowed: caller inputs; moved: an owned value at its last-use output (M2); reused: an operand at its last use (M6); never materialized: a fused intermediate (M7).
- There is **no** arena/workspace abstraction in the repository. Page-fault counting exists only in benchmarks (`getrusage`).
- `FLT_EVAL_METHOD == 0` on the measurement toolchains (GCC 13.3, Clang 18.1, x86-64 SSE2), so f32 pairs are fusible; no fast-math or reassociation flags are used.

### 4. Plan space (only what current semantics support)

Three execution plans exist today, each a combination of two already-implemented mechanisms:

| Plan | Reuse (M6) | Fusion (M7) | Meaning |
| --- | --- | --- | --- |
| P5 | off | off | every add allocates and writes its own result (materialized intermediate) |
| P6 | on | off | an add whose operand is owned and at its last use writes into that operand's storage |
| P7 | on | on | P6, plus strictly eligible adjacent add pairs run as one loop |

P7 is exactly today's default executor. P5 and P6 are **not reachable** through the current code path, so E1 adds the smallest experimental seam: an internal `ExecutionPolicy {reuse, fuse}` and `ExecutionStats` entry point (`src/runtime/execution_policy.hpp`), with default behaviour unchanged. No other production change is made. Fusion without reuse is deliberately not tested: it is dominated by P7 by construction (same passes, more allocations) and would only dilute the decision.

A plan is **valid** for a program if the policy produces it. Two policies that produce an identical execution (same counts of fresh results, in-place results, fused pairs, output copies, output moves, taken from the executor's own stats on an untimed dry run) are **one distinct plan** and are measured once. The distinct plan set per program is therefore determined by the executor, not by hand; the expected sets are in §5.

### 5. Workloads (the existing M5–M7 program family, plus the minimum needed to vary depth and dtype)

All programs use only `input`, `add`, `output`; length N for every input; seed 42; inputs from the oracle's generators.

| Id | Program | dtype | Expected distinct plans |
| --- | --- | --- | --- |
| single | `out(A+B)` | f32 | 1 (control) |
| final_left | `out((A+B)+C)` | f32 | P5, P6, P7 |
| final_right | `out(C+(A+B))` | f32 | P5, P6, P7 |
| inter_out | `D=A+B; out D; E=D+C; out E` | f32 | P5, P6 (P7≡P6) |
| after_use | `D=A+B; E=D+C; out D; out E` | f32 | 1 (control; reuse and fusion blocked) |
| chain3 | `((A+B)+C)+F` | f32 | P5, P6, P7 |
| chain4 | `(((A+B)+C)+F)+G` | f32 | P5, P6, P7 |
| final_left_i32 | `out((A+B)+C)` | i32 | P5, P6, P7 |
| chain3_i32 | `((A+B)+C)+F` | i32 | P5, P6, P7 |

A **decision cell** is a cell whose program has ≥ 2 distinct plans. Single-plan programs (`single`, `after_use`) are controls and are reported but excluded from the gates, because they cannot show regret by construction (including them would dilute every rate toward "no opportunity"). If the executor's stats show a different distinct-plan set than the table, the stats win and the difference is reported.

### 6. Size and regime axes

**Sizes N (elements)**: 64, 1 Ki, 4 Ki, 16 Ki, 64 Ki, 256 Ki, 1 Mi, 4 Mi, 16 Mi for all programs; 64 Mi additionally for `final_left` and `chain3` only (memory and time). The VM reports L1d 192 KiB total (4 cores), L2 8 MiB total and L3 260 MiB, but virtualized cache topology is not reliable, so these are not used to define size classes; the sweep is logarithmic and "cache-resident vs DRAM-scale" is judged only from the sweep itself. Working sets at 64 Mi are ≥ 1 GiB.

**Allocation/memory regimes** (environmental, not plan decisions):

| Regime | What it actually is |
| --- | --- |
| glibc_default | current allocator behaviour, default tunables |
| glibc_t1 | `GLIBC_TUNABLES=glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432` (no trimming, no mmap below 32 MiB: allocator steady state) |
| glibc_t2 | `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072` (every result-sized block is a fresh mapping) |
| arena_cold | result-sized allocations come from one pre-sized, reusable workspace; before every timed sample the workspace pages that were used are released with `madvise(MADV_DONTNEED)`, so the sample pays first-touch (demand-zero) page faults but no allocator syscalls |
| arena_warm | same workspace; its pages are resident (touched by an untimed priming interval and never released), so the sample pays neither allocator calls nor page faults for result storage |

The workspace is a measurement control, implemented as a size-class pool behind a replaced `operator new` that serves only result-sized requests (≥ 4·N bytes) while armed. It requires no change to `OwnedArray` or `Buffer`. Small bookkeeping allocations (the executor's index vectors) still use malloc in every regime. What each regime measures is documented in Part II; "page faults eliminated" is claimed only if measured.

Machine architecture is **not** a testable axis here (one VM). Any statement about other machines is NOT VERIFIED.

### 7. Cells and measurement protocol

- **Cell** = program × N × regime.
- **Sample** = one timed interval of K executions (K = 1000 for N < 4096, else 1, as in M1–M7), results held until the interval ends and checked against the oracle **outside** the timed region; per-call time = interval / K.
- **Priming**: before every timed sample of a plan an untimed interval of the same plan runs (so each plan is measured in its own allocator steady state, as in M1–M7, while plans are still interleaved). For arena_cold the pages are released between the priming interval and the timed one; for the workspace regimes the workspace is reset between intervals.
- **Repetitions**: 15 timed samples per plan per cell per replicate; plan order is rotated every repetition (Latin-square rotation) so no plan is systematically first or last.
- **Replicates**: the full matrix is run as **3 independent process launches** (replicates) at different times. Samples are keyed by (replicate, repetition); a "unit" is one (replicate, repetition) with all plans measured adjacent in time.
- **Twin control (noise calibration)**: in every cell the fixed-policy plan is measured a second time under the label `FIXED_TWIN`, interleaved like any other plan, and **never** counted as a strategy. Its apparent regret against the original quantifies how much "regret" pure measurement noise (and winner selection) manufactures in that cell.
- **Statistic**: median per plan; spread as IQR; paired comparisons use per-unit ratios. Raw samples (every timed interval) are recorded; **no outlier is discarded**.
- **Counters** recorded per sample: minor and major page faults (`getrusage`), user/system CPU time (`getrusage`, coarse), result-sized allocation count, bytes and peak live bytes (replaced `operator new`, result-sized requests only), arena capacity/high-water/fallbacks, and the executor's own `ExecutionStats` (fresh results, in-place results, fused pairs, output copies, output moves). Logical payload bytes per element and passes over N are **CALCULATED** from those stats (add loop 12 B; fused pair loop 16 B; output copy 8 B; output move 0; passes = loops + output copies), not measured. No hardware counters are expected (virtualized); none will be invented.
- **Compiler/flags**: GCC 13.3 and the repository's Release flags (`-O3 -DNDEBUG`), no `-march`, no fast-math; every flag, compiler version, kernel, CPU model, glibc version, THP setting and the commit SHA are written to a metadata file. A Clang 18 replicate of the full matrix is run if time permits, reported separately and never pooled with GCC.
- **Equal engineering effort**: all plans run through the same executor function with the same compiler flags, the same inputs and the same harness code path; only the policy flags differ.

### 8. Correctness (a failure invalidates the cell)

Every timed result of every plan is compared exactly to the independent oracle's output (bit-exact for non-NaN f32; NaN matches NaN; i32 exact). A mismatch aborts the run and the cell is invalid. In addition the repository test suite, the existing mutation-style tests, sanitizer configuration, and new tests for the seam (all policies × all programs × edge N) and for the workspace (alignment, capacity boundary, no overlap of live blocks, reset/reuse, repeated runs, small N, large N) must pass.

### 9. Definitions

- **Fixed policy** `ALWAYS_REUSE_AND_FUSE` = executor policy {reuse=on, fuse=on} = P7 wherever legal; where fusion/reuse is not legal the same policy yields the plan the executor can legally run (P6 for `inter_out`).
- **Median time** of a plan in a cell = median over all pooled timed samples (3 replicates × 15 repetitions = 45 per plan, ns per call).
- **Regret(plan, cell)** = median(plan) / median(empirical best distinct plan); the empirical best is the distinct plan with the lowest median time (the twin is excluded). Fixed-policy regret ≥ 1 by construction.
- **Noise-corroborated opportunity cell**: a decision cell is an *opportunity cell* if some distinct plan q satisfies (i) median(fixed)/median(q) > 1.05 and (ii) the lower end of the 95 % bootstrap interval (1000 resamples of units, paired per unit) of the median of per-unit ratios t_fixed/t_q is > 1.00. This is the preregistered reading of "a 0.5 % win is not automatically meaningful": criterion A counts only cells whose > 5 % regret is statistically supported. The raw count of cells with point regret > 1.05 is reported next to it.
- **Decisive win** in a cell (for flips): one distinct plan has median ≤ 0.97 × the median of every other distinct plan, and is faster than each of them in ≥ 75 % of the units.
- **Stable meaningful regime flip**: for the same program, two cells that differ in exactly one axis (allocator regime, or size) have different decisive winners W1 ≠ W2, **and** running W1 in the second cell costs ≥ 5 % (median ratio ≥ 1.05), **and** the same pair of decisive winners appears when each replicate is analysed on its own (decisive in each of the 3 replicates separately, using its 15 units and the same rules). "Size" flips are reported separately from "allocator regime" flips; either counts for criterion C.

### 10. Gates (thresholds fixed here; applied without modification)

Computed over **decision cells**, pooled replicates:

- **A. Opportunity rate**: ≥ 20 % of decision cells are noise-corroborated opportunity cells.
- **B. Tail regret**: p95 of fixed-policy regret over decision cells ≥ 1.15 (raw point estimates).
- **C. Stable regime flip**: at least one stable meaningful regime flip exists.

**Planning remains interesting (hypothesis survives) iff at least one of A, B, C holds.**

**Kill condition** (a stricter statement than "none holds"): ≥ 95 % of decision cells have fixed-policy regret ≤ 1.05 **and** p95 regret < 1.15 **and** no stable meaningful flip. Verdict mapping: any of A/B/C ⇒ `PLANNER HYPOTHESIS SURVIVES`; none of A/B/C ⇒ `PLANNER HYPOTHESIS FAILS FOR THIS TESTED SPACE` (reporting whether the kill condition is also strictly met). Noise-adjusted context is reported but does not change this mapping: the twin's null distribution of regret is shown beside every regret statistic.

If a replicate is invalidated or the Clang replicate disagrees, that is reported; the gate is evaluated on the GCC pool.

### 11. Predictions made before measurement (so they can be wrong)

Written after reading the M5–M7 records, before any E1 measurement, and disclosed as informed by them:

1. P7 is the best or tied-best plan in the large majority of decision cells; P5 never beats P7 by a decisive margin at N ≥ 256 Ki.
2. Fixed-policy regret is within ≈ 5 % in ≥ 90 % of decision cells; median regret ≈ 1.00.
3. The largest differences are at large N under glibc_default (allocator effects favouring reuse and fusion), which the arena regimes shrink; so allocation effects explain much of the M5/M6 gap and little of the M7/M6 gap.
4. At very small N (64, 1 Ki) all plans are within noise.
5. Predicted verdict: **FAILS FOR THIS TESTED SPACE**. The plan space is nearly ordered by construction (P7 has the fewest passes and allocations), which is a limitation of the test, stated in advance: a low regret here says little about richer spaces.

### 12. Evidence classification used in Part II

MEASURED (observed), CALCULATED (from stats/model), INFERRED (reasoned, not isolated), NOT VERIFIED (not tested here, e.g. other machines).

### 13. Threats known in advance

One shared KVM VM (drift of ±10 % for identical code was observed in M7); no PMU; virtualized cache topology; allocator behaviour depends on glibc version and allocation history; the workspace regime is a replaced global `operator new` (a different allocator, not a runtime arena inside Lume); regimes share one process per launch; the plan space is tiny and ordered; sizes and programs are my choice; the benchmark checks every result, which changes cache state between samples (as in M1–M7); small bookkeeping allocations are not in the workspace.

### 14. Commit order (verifiable in `git log`)

1. this preregistration;
2. the workspace/arena control;
3. its tests;
4. the executor policy seam and its tests;
5. the harness and analysis script;
6. the data;
7. the report.

---

## Part II — Results

*Written after the data were collected. Part I above is unchanged since the preregistration commit `ded583b`. Full analysis: [`e1-report.md`](e1-report.md); data and code: [`e1-results/`](e1-results/), `experiments/e1-is-planning-necessary/`.*

### Outcome against the preregistered gates (GCC 13.3, 3 replicates, pooled)

| Statistic | Value |
| --- | ---: |
| Decision cells | 325 (415 cells in total) |
| Fixed policy is the lowest-median plan | 324 of 325 |
| Cells with fixed regret > 1.05 (raw) | 0 |
| Noise-corroborated opportunity cells (gate A) | 0 (0 %) |
| Median / p95 / maximum fixed regret (gate B uses p95) | 1.000 / 1.000 / 1.038 |
| Stable meaningful regime flips (gate C) | 0 (and 0 cell pairs with differing decisive winners) |
| Twin noise floor (median / p95 / max "regret" of an identical plan) | 1.003 / 1.026 / 1.117 |

Gates A, B and C are all false; the kill condition is strictly met. Clang (separate 3 replicates): max regret 1.009, no flips. Post-hoc fresh-process default allocator regime (supplementary): 65 decision cells, max regret 1.022, no flips.

**Verdict: PLANNER HYPOTHESIS FAILS FOR THIS TESTED SPACE.** The plan space is totally ordered by construction (P7 dominates P6 dominates P5 in passes and allocations); the result is about this space only.

### Deviations from Part I (all disclosed in the report)

1. The arena and its tests were committed together (`8a2e86a`), not as two commits.
2. A first full run was **superseded** after an instrument defect was found (the allocation hook's cost grew with the number of held results, inflating small-N glibc timings by ~1 µs per call). The hook was fixed (`84706f9`) and the whole matrix rerun with identical rules and thresholds. The first run is kept unedited in `e1-results/superseded_v1_hook_scan/`; its gate outcome was the same.
3. A **post-hoc supplementary regime** `glibc_default_fresh` (one process per cell) was added after a diagnostic showed the in-matrix `glibc_default` regime is allocator-history dependent. It is reported separately and does not enter the preregistered gates.
4. The Clang replicate set permitted by §7 was run (3 replicates), reported separately.
5. Implementation choices that Part I left open: p95 by nearest rank (the linear definition gives the same outcome); bootstrap = 1000 resamples with a fixed seed, lower end = 2.5th percentile; `FIXED_TWIN` regret defined as median(fixed) / min(median(fixed), median(twin)).
6. Commit messages cite "64,584 samples"; the exact count is 64,575 samples per set (plus one header line per file).
7. Per-replicate regret, alternative-policy regret and the `ns per fault` / bandwidth tables are post-hoc descriptive additions and are labelled as such.
