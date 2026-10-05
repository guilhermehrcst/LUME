# E1 report — Is planning necessary?

Preregistration: [`e1-is-planning-necessary.md`](e1-is-planning-necessary.md) (Part I, frozen at `ded583b`). Data: [`e1-results/`](e1-results/). Code: `experiments/e1-is-planning-necessary/`.

Evidence labels: **MEASURED** (observed), **CALCULATED** (from the executor's own counts or a stated model), **INFERRED** (reasoned, not isolated), **NOT VERIFIED** (not tested here).

## A. Executive verdict

**PLANNER HYPOTHESIS FAILS FOR THIS TESTED SPACE.**

In the plan space the current executor can express (P5 materialize, P6 + last-use reuse, P7 + strict fusion), the fixed policy `ALWAYS_REUSE_AND_FUSE` (= today's default executor) was the lowest-median plan in 324 of 325 decision cells and never lost by more than 3.8 %. All three preregistered continue-gates are false, and the kill condition is strictly met. Clang, a per-replicate view and a post-hoc fresh-process default allocator regime agree.

This does **not** say planning is useless in general. The tested space is totally ordered by construction (each step from P5 to P7 does fewer passes and fewer allocations), so a fixed rule "do every legal optimization" is trivially optimal in it. E1 shows that **this** space contains no decision; it does not show that richer spaces (shared intermediates, recompute, encodings, parallelism, other hardware) contain none. The decision still matters a great deal in the other direction: a *wrong* fixed policy would be badly wrong (median regret 1.28–1.85 for always-materialize, 1.07–1.16 for reuse-only; MEASURED, post hoc). The finding is "the right answer is always the same, and statically knowable", not "the answer does not matter".

## B. Environment

| | |
| --- | --- |
| Machine | KVM guest, 4 vCPU, Intel Xeon @ 2.10 GHz, 16 GiB RAM; **virtualized: no hardware performance counters** |
| Reported caches | L1d 192 KiB total, L2 8 MiB total, L3 260 MiB (virtualized topology, not relied upon) |
| OS | Linux 6.18.44, glibc 2.39, transparent huge pages `madvise` |
| Compilers | GCC 13.3.0 (primary), Clang 18.1.3 (separate replicate set) |
| Flags | CMake Release: `-O3 -DNDEBUG`; no `-march`, no fast-math, no reassociation; `FLT_EVAL_METHOD == 0` |
| Base | `main` = `e046b6f66a8f3708f205984648149b021a172859` (CI green; no open PRs) |
| Runtime under test | `src/runtime/cpu_reference.cpp` at that base plus the E1 seam (below) |

i32 add wraps modulo 2^32 (defined: `wrapping_add` via `uint32_t`), and the oracle (`lume_oracle`, an `INTERFACE` library) does not link the runtime — both verified from the code, not copied from earlier reviews.

## C. Pre-registered hypotheses

- **H0**: a fixed policy (`ALWAYS_REUSE_AND_FUSE`) is close enough to optimal that a planner is unnecessary for the tested plan space.
- **H1**: valid strategies win in enough regions that the fixed policy has meaningful regret.
- **Continue gates (any one)**: A ≥ 20 % of decision cells are noise-corroborated opportunity cells (fixed regret > 1.05 with a paired-bootstrap lower bound > 1); B p95 fixed regret ≥ 1.15; C a stable, meaningful ranking flip across regimes or sizes.
- **Kill (stricter)**: ≥ 95 % of cells within 5 %, p95 < 1.15, no flip.

Predictions made before measurement and their outcome: (1) P7 best or tied, P5 never decisively ahead at N ≥ 256 Ki — **supported**; (2) regret within ≈ 5 % in ≥ 90 % of cells — **supported (100 %)**; (3) the largest differences are allocator effects at large N under default glibc, shrinking in the arena regimes — **partly**: true for P5 versus P6/P7, but only under the fresh-process default (§F); (4) at N = 64 and 1 Ki all plans are within noise — **falsified**: in the no-fault regimes P5 is 13–23 % (N = 64) and 25–31 % (N = 1 Ki) slower than P7 (the first, flawed run had hidden this, §J); (5) verdict fails — **supported**.

## D. Arena design

A measurement control, not a runtime feature. `experiments/e1-is-planning-necessary/workspace.hpp`: one anonymous mapping reserved up front (`MAP_NORESERVE`), 64-byte-aligned blocks with a 64-byte header, bump allocation plus per-size LIFO recycling (as malloc would recycle a just-freed buffer), `reset()`, `prefault()`, `release_pages()` (`MADV_DONTNEED`). It fails closed: `allocate` returns `nullptr` instead of overrunning, `deallocate` aborts on a foreign pointer or double free, `reset`/`release_pages` refuse while blocks are live. `alloc_hooks.cpp` replaces the global `operator new/delete`; while armed it counts and (optionally) routes only requests of **exactly** the result size (4·N bytes) to the workspace. `OwnedArray`, `Buffer` and the executor are untouched by the arena.

| Regime | What it measures (MEASURED unless stated) |
| --- | --- |
| `glibc_default` | default allocator, **inside one long-lived process** (see §J: the allocator's dynamic thresholds had been adapted by earlier cells, so mid-size cells behave like T1) |
| `glibc_default_fresh` | *post hoc, supplementary*: default allocator, one process per cell; reproduces the M5 page-fault regime |
| `glibc_t1` | no trimming, no mmap below 32 MiB: allocator steady state |
| `glibc_t2` | every result-sized block is a fresh mapping: every buffer faults on first touch |
| `arena_cold` | workspace, pages released before each timed sample: pays first-touch faults (≈ 0.9–1.0 µs/page up to 1 Mi, 1.36–1.65 µs/page from 4 Mi), no allocator calls |
| `arena_warm` | workspace, pages resident: no allocator calls and 0 page faults in every cell (MEASURED: 0 faults/call) |

Not eliminated by the arena: the executor's small index vectors (still malloc), the cost of the hook itself (equal for all plans), 64-byte versus 16-byte payload alignment (differs between arena and malloc regimes, identical across plans). At N ≥ 16 Mi the buffers are ≥ 64 MiB and glibc maps them whatever the tunables, so the three glibc regimes coincide there.

The executor seam: `src/runtime/execution_policy.hpp`, an internal `ExecutionPolicy {reuse, fuse}` and `ExecutionStats`; `execute_cpu_reference` is unchanged in behaviour (the default policy). P5 = {off, off}, P6 = {on, off}, P7 = {on, on}.

## E. Correctness

- Every one of the 142,065 timed samples of the reported runs (64,575 GCC + 64,575 Clang + 12,915 fresh) was compared exactly with the independent oracle outside the timed region; **0 mismatches, 0 workspace fallbacks, 0 tracking overflows** (MEASURED).
- The harness also checks, in an untimed dry run per plan, that the hook's result-sized allocation count equals the executor's own `fresh_results + output_copies` (CALCULATED vs MEASURED agree in every cell, else the run aborts).
- Tests (all in CTest, 27/27 on GCC Release `-Werror`, Clang Release `-Werror`, GCC ASan+UBSan `-Werror`): `lume_policy_test` (3 policies × 7 program shapes × f32/i32 × 13 lengths against the oracle, the stats of each plan, input immutability, error paths; both policy flags are mutation-sensitive: ignoring either makes it fail); `lume_e1_workspace_test` (alignment, exact capacity boundary, no overlap of live blocks, LIFO reuse, reset/refusal, repeated runs, size 0/1, 64 MiB block, page release reads zero, class-table overflow); `lume_e1_hooks_test` (routing, exact-size matching, counters, exhaustion fallback, ownership after disarm, 3000 live blocks freed in three orders, table overflow reporting); `lume_e1_bench_smoke` (every program, three regimes, oracle checks). Valgrind: clean on `policy_test` and `workspace_test`; not run on the tests that replace `operator new` (its own interposition conflicts).
- Not re-run: the historical manual mutation scripts of M7 (the default policy path is unchanged and covered by the existing M5–M7 tests, which pass).

## F. E1-A results — what is allocation, first touch and page faults?

GCC, `final_left` (`(A+B)+C`), medians pooled over 3 replicates × 15 repetitions. Ratios of slower/faster plan:

| N | regime | P5/P6 | P6/P7 | P5/P7 |
| ---: | --- | ---: | ---: | ---: |
| 64 | glibc_default / arena_warm | 1.06 / 1.05 | 1.07 / 1.10 | 1.14 / 1.15 |
| 1 Ki | glibc_default / arena_warm | 1.05 / 1.05 | 1.24 / 1.23 | 1.31 / 1.29 |
| 64 Ki | fresh default | 6.79 | 1.18 | 7.97 |
| 64 Ki | glibc_default / T1 / arena_warm | 1.12 / 1.10 / 1.06 | 1.18 / 1.19 / 1.19 | 1.32 / 1.32 / 1.26 |
| 1 Mi | fresh default | **3.55** | 1.27 | **4.50** |
| 1 Mi | glibc_default / T1 / **arena_warm** | 1.21 / 1.21 / **1.21** | 1.26 / 1.27 / **1.27** | 1.52 / 1.54 / **1.54** |
| 1 Mi | T2 / arena_cold | 1.86 / 1.68 | 1.11 / 1.13 | 2.07 / 1.91 |
| 4 Mi | fresh default / arena_warm | 3.93 / 1.44 | 1.35 / 1.27 | 5.31 / 1.83 |
| 16 Mi | all glibc regimes / arena_warm | 1.56 / 1.16 | 1.17 / 1.35 | 1.83 / 1.57 |
| 64 Mi | all glibc regimes / arena_warm | 1.55 / 1.15 | 1.16 / 1.37 | 1.80 / 1.58 |

Answers to the E1-A questions (MEASURED unless marked):

1. **How much does ARENA_WARM change M5 (P5)?** Against allocator steady state (default-in-matrix, T1): within ±10 % for N ≤ 4 Mi (0.98–1.10×), 2.3–2.5× at N ≥ 16 Mi where the glibc regimes fault on every call. Against the fresh-process default: 3.0× (1 Mi), 3.2× (4 Mi), 6.5× (64 Ki), 1.3× (16 Ki). Against T2/cold: 3–9× (N ≥ 1 Ki).
2. **M6 (P6)?** Fresh default ≈ arena_warm (1.00–1.19×); steady ≈ 1.0×; fault regimes 2.2–5.3×; 1.7–1.8× at N ≥ 16 Mi.
3. **M7 (P7)?** Same pattern as P6 (1.01–1.25× fresh default, the top end at 16 Ki; 2.4–5.9× in fault regimes; 2.0–2.1× at ≥ 16 Mi). P6 and P7 allocate one result buffer; P5 allocates two, so first-touch cost per call is ×1, ×1, ×2 (CALCULATED from faults/call: 1025, 1025, 2049 at 1 Mi).
4. **Does the M7/M6 ranking change?** No. P6/P7 > 1 in every regime from 1 Ki up (1.26–1.27 at 1 Mi in the no-fault regimes; 1.11–1.13 in T2/cold, because the first-touch cost is common to both and dilutes the ratio). The single exception is one cell (N = 16 Ki, `arena_cold`: 0.963), inside the noise floor (§H).
5. **Does the M6/M5 ranking change?** No. But its size depends on the regime by a factor of three: P5/P6 = 1.21 at 1 Mi with the allocator controlled, 3.55 in the fresh-process default.
6. **Do differences shrink substantially?** The *M5→M6* difference does (3.55× → 1.21× at 1 Mi; 6.8× → 1.06× at 64 Ki). The *M6→M7* difference does not (1.27× in fresh default, T1 and warm arena alike).
7. **What remains after allocation/page effects are controlled?** For N ≥ 256 Ki in the no-fault regimes: P5/P6 ≈ 1.2–1.45× up to 4 Mi and 1.15–1.16× at ≥ 16 Mi, P6/P7 ≈ 1.2–1.37×, P5/P7 ≈ 1.5–1.8×. At DRAM scale the warm-arena times scale with the modeled payload (P5/P7 = 1.57–1.58 at 16–64 Mi versus the model's 24/16 = 1.5; CALCULATED model, MEASURED times). P5/P6 stays at 1.15 there although both are modeled at 24 B/element — the in-place pass is cheaper than the model says, as M6 found (INFERRED mechanism, not counter-verified).
8. **Is the runtime now memory-traffic/computation dominated?** In `arena_warm`, logical bandwidth (CALCULATED payload ÷ MEASURED time) is 12.7 / 14.7 / 13.3 GB/s (P5/P6/P7) at 16–64 Mi, i.e. one memory-bound plateau for all plans; 26–31 GB/s at 1 Mi; 55–93 GB/s at 4–64 Ki (cache-resident); and overhead-dominated below 1 Ki (≈ 250 ns per call at N = 64). The payload is a model, not DRAM traffic (no counters).
9. **Was a previous interpretation confounded by allocator/page behavior?** Yes, one: the headline default-glibc numbers of M5/M6 (4.3×, 0.28×) are allocator-history dependent. The same executor and plans reproduce them in a fresh process (P5 at 1 Mi: 2,016 faults/call, 2.9–3.2 ms) and do not reproduce them in a long-lived process whose earlier cells had adapted glibc's dynamic thresholds (0 faults, 0.97 ms). The M6 inference that the T1 gain is not an allocator effect, and the M7 result (≈ 0.8×, independent of regime), are **not** contradicted: with the allocator controlled, P6 is still 1.21× faster than P5 and P7 1.27× faster than P6. *Correlation versus cause*: that the allocator explains the fresh-default cliff is INFERRED from the fault counts and the arena/T1 controls (strongly, since arena_warm removes it), not isolated by an intervention inside glibc.

## G. E1-B opportunity map

415 cells per compiler: 9 programs × 9–10 sizes × 5 regimes (325 decision cells with ≥ 2 distinct plans; 90 single-plan control cells for `single` and `after_use`). Per cell: P5, P6, P7 as distinct executions (merged when the executor's stats are identical, e.g. `inter_out` where P6 ≡ P7) plus the `FIXED_TWIN`. 3 replicates × 15 repetitions → 45 interleaved samples per plan per cell. Raw matrix: 64,575 samples (GCC), 64,575 (Clang), 12,915 (fresh-process supplement). Machine-readable: `e1-results/*/raw_*.csv`, `derived/cells.csv` (cell, empirical best, fixed time, best time, regret, second best and its regret, ranking, twin regret, opportunity flag and CI), `derived/flips.csv`, `derived/gates.json`, `derived/summary.md`, `derived/e1a_tables.md`.

Empirical-best plan (GCC): P7 in 279 cells, the P6≡P7 group in 46 (45 `inter_out` cells and one `final_left` cell); P5 in none. Decisive winners (≥ 3 % ahead of every rival and ahead in ≥ 75 % of units): P7 256, the P6≡P7 group 44, none 25. Rankings (fastest first) of the three-plan programs are P7 < P6 < P5 in 279 of 280 GCC cells; the exception is `final_left` N = 16 Ki `arena_cold` (P6 3.8 % ahead of P7). In the Clang set 269 of 280 follow it, 9 swap P5 and P6 (all at N = 1 Ki, where they differ by < 5 %) and 2 put P6 ahead of P7 by ≤ 0.9 % at N = 64 `arena_cold`.

## H. Regret analysis

| | GCC (primary) | Clang | fresh-process default (suppl.) |
| --- | ---: | ---: | ---: |
| Decision cells | 325 | 325 | 65 |
| Fixed policy is the lowest-median plan | 324 | 322 | 64 |
| Cells with fixed regret > 1.05 (raw) | **0** | 0 | 0 |
| Noise-corroborated opportunity cells | **0** (0 %) | 0 | 0 |
| Median regret | **1.000** | 1.000 | 1.000 |
| p95 regret (nearest rank; linear gives the same) | **1.000** | 1.000 | 1.000 |
| Maximum regret | **1.038** | 1.009 | 1.022 |
| Share of cells within 5 % | 100 % | 100 % | 100 % |
| Stable meaningful flips | **0** | 0 | 0 |
| Cell pairs whose decisive winners differ (any strength) | 0 | 0 | 0 |
| Twin (identical-plan) "regret": median / p95 / max | 1.003 / 1.026 / 1.117 | 1.003 / 1.021 / 1.071 | 1.005 / 1.051 / 1.156 |

The twin row is the noise floor: an identical plan measured twice in the same cell appears up to 12–16 % "slower" and exceeds 5 % in 1.2 % (GCC) and 6 % (fresh, only 65 cells) of cells with no real difference at all. The largest fixed-policy regret in any run (1.038) is inside it. In GCC the maximum fixed regret is exactly 1.000 (the fixed plan has the lowest median) in every regime except `arena_cold`, every program except `final_left` and every size except 16 Ki; all of the excess above 1 comes from that single cell (1.038). Viewed one replicate at a time (15 repetitions each), single cells reach 1.05–1.13 (GCC) and 1.10 (Clang) — disturbances of one series, visible because the twin moves with them — and the medians are still 1.000; the pooled analysis is the preregistered one.

*Post hoc, descriptive (not a gate): the instrument can see regret when there is some.* Applying the same measurement to other fixed policies, regret against the empirical best (GCC, per regime): always-materialize (P5) median 1.28–1.85, p95 1.83–2.84, max 1.97–3.52, > 5 % in 97–98 % of cells; reuse-only (P6) median 1.07–1.16, p95 1.21–1.33, max up to 1.37, > 5 % in 66–83 % of cells. Either would pass gates A and B immediately.

## I. Threshold decision

| Gate | Threshold | Observed (GCC) | Holds? |
| --- | --- | ---: | :-: |
| A opportunity rate | ≥ 20 % corroborated | 0 % | no |
| B tail regret | p95 ≥ 1.15 | 1.000 | no |
| C stable regime flip | ≥ 1 | 0 | no |
| Kill condition | ≥ 95 % within 5 %, p95 < 1.15, no flip | 100 %, 1.000, 0 | **met** |

Verdict: **PLANNER HYPOTHESIS FAILS FOR THIS TESTED SPACE.** Clang and the supplementary run give the same verdict; neither the rules nor the thresholds were changed after seeing data.

## J. Threats to validity

- **The tested space is monotone by construction** (§A). This is the strongest limitation: the experiment could only have failed to confirm "no decision" if some mechanism hurt somewhere, and none did.
- **An instrument defect was found and fixed mid-experiment.** The first full run used a hook that scanned a table of live blocks on every free; with K = 1000 held results that added ≈ 1 µs per call in the glibc regimes at N < 4096, compressing differences exactly there (it made all plans look within noise at tiny N, which is how prediction 4 first appeared to hold). It was caught by comparing N = 64 with the M7 benchmark (≈ 265 ns), fixed with an O(1) hash set, and the whole matrix was rerun. The first run is kept unedited in `e1-results/superseded_v1_hook_scan/`; its gate outcome was the same, but its small-N timings are unusable. The reported results are from the rerun. The choice to rerun was made because the flaw biased toward "no regret"; it could only increase detected regret.
- **The in-matrix `glibc_default` regime is history dependent** (found after the first analysis): it behaved like T1 for 256 Ki–4 Mi because earlier cells had raised glibc's dynamic mmap/trim thresholds. The fresh-process supplement fixes the representativeness question but was added post hoc and is reported separately; gates use only preregistered regimes (the fresh run's gates are also reported and pass nothing).
- **The "arena" is a replaced `operator new` serving result-sized blocks, not an in-runtime arena.** It tests allocation/page effects, not what an arena-aware planner would do (for example reuse across calls with lifetime knowledge). 64-byte versus 16-byte payload alignment differs between arena and malloc regimes (not between plans).
- **One virtual machine**, noise of ±10 % even for identical code at the series level, no hardware counters, virtualized cache topology, THP `madvise`. The machine axis is **NOT VERIFIED** (not testable here); so are other CPUs, ARM, other OSes, other allocators (jemalloc, tcmalloc, mimalloc), multithreading, AVX (no `-march`).
- **Interval design**: K = 1000 held results for N < 4096 (as in M1–M7) means "cold" there is first-call-only; each timed sample is preceded by a priming interval of the same plan rather than a long steady loop; plans share a process per regime.
- **Statistics**: pooled medians over 45 samples; a 1000-resample paired bootstrap with a fixed seed; nearest-rank p95 (not specified in the preregistration; the linear definition gives identical gate outcomes); a winner's-curse bias in "regret versus the best of several noisy plans" that the twin quantifies.
- **Sizes and programs are my choice**; 64 Mi only for two programs (memory/time).
- Allocation accounting depends on exact-size matching (4·N bytes) and is cross-checked against the executor's stats in every cell.

## K. What was falsified

- H1 for this space: the fixed policy is not meaningfully suboptimal anywhere, in any allocator regime, size or dtype.
- "Execution-plan ranking flips between allocator regimes or between cache- and DRAM-scale sizes": no flip at any strength.
- "Plans are indistinguishable at tiny N" (prediction 4): in the no-fault regimes P5 takes 13–31 % longer than P7 at N = 64 … 1 Ki.
- "The M6/M7 results are artifacts of default-glibc behavior": not for M7 (regime-independent 1.27×) and not for the in-place effect of M6 (1.21× with the allocator controlled); but the *magnitude* of the M5/M6 default-glibc headline is allocator-history dependent (3.5–8× in a fresh process, 1.0–1.3× in a process that has adapted its thresholds).

## L. What survived

- The ordering P7 < P6 < P5 and its size, once allocation is controlled (1.2 / 1.27 / 1.54 at 1 Mi), across GCC and Clang, f32 and i32, left and right consumers, depth 3–4.
- The accounting by number of fresh buffers: first-touch cost ≈ 0.9–1.0 µs/page (≤ 1 Mi), ≈ 1.36–1.65 µs/page (≥ 4 Mi), linear in buffers written (faults/call 1025 / 1025 / 2049 for P6 / P7 / P5 at 1 Mi).
- The measurement instrument: twin control, paired bootstrap, exact-oracle checking and accounting cross-checks; it reports large regret for wrong fixed policies.
- The finding that allocator history can swing a single plan 3–8× is itself a robust, reproducible result, and the reason an allocator-aware planner (or an allocator-independent executor) is worth keeping in mind.

## M. Recommended next action

Do **not** build the planner, the cost model or the analysis→plan→verify pipeline on the strength of this space (the Phase 8 design note is not triggered; nothing here showed which Facts matter, because nothing varied).

Run **E2: does the best choice between materializing a shared intermediate and recomputing it depend on the machine regime, or only on a static fact?** Preregistered with the same gates, harness, twin control and regimes. The current IR already expresses it: a program in which one intermediate `D = A + B` feeds k consumers, `E_i = D + C_i` for i = 1…k, all outputs. The two plans are **materialize D once** (one pass for D, then k passes that read D; the last consumer can reuse D in place) and **recompute D inside each consumer** (k fused passes). By the payload model the cost is 12 + 12k versus 16k bytes/element: recompute wins for k = 2 (36 vs 32), they tie at k = 3 (48), and materialize wins for k ≥ 4 (60 vs 64), so, unlike P5–P7, neither plan dominates. E2 asks the question E1 could not: does the crossover k* move with N and the allocation/memory regime (then a cost model over machine facts is justified) or is it fixed (then a static rule on fan-out suffices and the planner thesis loses its best candidate inside the current IR)? Needs one new experimental mechanism (recompute-fusion across consumers) behind the same policy seam, and nothing else. Do not build the planner, a cost model or encodings before E2 reports.
