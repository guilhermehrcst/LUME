# E3 - research design draft (NOT a preregistration)

Status: **draft for discussion. Nothing here is frozen, implemented or run.** A preregistration may be written only after E2-R1 has a verdict, and only for the branch that verdict selects (section 2). Numbers below are proposals to be argued over, not commitments.

## 1. Question

> Under what conditions does one verified computation admit several legal physical execution plans with meaningfully different costs, and can the best plan be predicted from facts known before running it, without fitting to the machine, sizes or program shapes it is tested on?

What E1 and E2 established (see their reports): in the reuse/fusion space of E1 the plans are ordered by construction, so there is no decision. In E2's materialize-vs-recompute space the decision is real but depended mainly on static facts (dtype, fan-out k, N); a per-(dtype, k, N) table fitted on one VM was within p95 1.006 of the best, and within p95 1.03 even when the memory regime was held out. Whether that table transfers to other machines is exactly what E2-R1 measures.

## 2. Dependence on E2-R1 (decided before any E3 design is frozen)

| R1 verdict | E3 branch | rationale |
| --- | --- | --- |
| INCONCLUSIVE | **no E3**; repair R1 | an unvalidated premise cannot support a follow-up |
| REGIME-SENSITIVE PLANNING SURVIVES | **E3-C**: replicate the regime effect first, then ask which runtime signal predicts it | E1/E2 make this the least expected outcome; it needs confirmation before anything is built on it |
| MACHINE-CALIBRATED STATIC POLICY | **E3-M** (main design below): can measured machine parameters replace per-machine timing tables? | the decision would then be static but machine-specific |
| PORTABLE STATIC POLICY SUFFICES | **E3-T**: does a portable rule survive richer graph topologies, where decisions interact? | E2's space is one star-shaped pattern; portability may not generalize |

Also carried from R1: the per-machine noise floor, whether E2's non-monotone N profile reproduces, compiler disagreement, PMU availability.

## 3. What is already known (prior art; no novelty is claimed)

- Recompute versus store is a central, first-class scheduling choice in Halide (`compute_inline` / `compute_root` / `compute_at`; Ragan-Kelley et al., PLDI 2013). "Materialize if shared, otherwise inline" is an established heuristic (e.g. Li et al., TOG 2018; PostgreSQL 12 inlines CTEs referenced once).
- XLA decides producer duplication in fusion with hand-written heuristics (`xla/service/instruction_fusion.cc`). An open issue reports XLA:CPU materializing large intermediates because of interacting fusion heuristics (openxla/xla#49131, opened 2026-09-20). An unmerged PR proposed a per-machine rule "recompute iff extra_flops <= machine_balance * saved_bytes" (openxla/xla#49316, closed 2026-09-23). I checked both pages directly.
- Learned schedule cost models degrade across hardware and need transfer or multi-task learning (TenSet, NeurIPS 2021 Datasets & Benchmarks; TLP, ASPLOS 2023). Empirical tuners assume per-machine tuning (ATLAS 2001; FFTW, Proc. IEEE 2005, whose manual warns that plans do not transfer across processors and can change with memory layout and recompilation).
- Analytical models can match search for some kernels (Yotov et al. 2005; Low et al. 2016). Measurement setup alone can flip performance conclusions (Mytkowicz et al., ASPLOS 2009).
- Roofline (Williams et al., CACM 2009) bounds memory-bound kernels by bandwidth; equality saturation (egg, POPL 2021; Tensat, MLSys 2021) extracts the cheapest equivalent program under a given cost model; translation validation (Pnueli et al. 1998; Alive2, PLDI 2021) certifies equivalence per compilation.

Plausible gap, stated cautiously (medium confidence; based on a quick literature search, and absence of evidence is weak evidence): a preregistered, falsifiable test of whether plan decisions for tiny element-wise graphs, chosen from a parameterized machine model rather than per-machine timing, generalize to held-out machines, sizes and topologies, with explicit overfitting controls. The contribution, if any, would be the measurement discipline, not the idea that decisions are machine-dependent.

## 4. E3-M design sketch (for the MACHINE-CALIBRATED branch)

**Plan space.** For each program, every legal physical plan obtained by choosing, per intermediate, materialize / recompute into each consumer / reuse a dying operand's storage / fuse with its single consumer. Legality is checked with the existing verifier and the independent oracle: i32 bit-exact, f32 bit-exact with no reassociation. A plan that is not bit-identical is illegal, not merely slower. Every plan is physically distinct (different executed loops), confirmed from the executor's stats, as in E1.

**Program families** (from a small grammar, frozen before data): star fan-out (E2 anchor), chains of depth d, diamonds, two-level sharing, mixed fan-out. Each family has a fixed set of sizes.

**Sizes.** E2's nine powers of two **plus non-power-of-two sizes** (for example N x 1.1 and N x 1.5). Every E2 and R1 size is a power of two, so cache-set aliasing cannot be separated from size there (HYPOTHESIS for E2's non-monotone points).

**Machines and compilers.** At least the R1 machines (x86-64 and ARM64 physical), ideally three or more for leave-one-machine-out; GCC and Clang never pooled.

**Predictors** (all frozen before data):

| ID | predictor | uses target timing? |
| --- | --- | --- |
| P0 | current default policy (reuse + fusion) | no |
| P1 | logical byte model (E2's 12/16 B per element) | no |
| P2 | parameterized machine model: bytes per level / bandwidth of the level the working set fits in, plus per-loop overhead. Parameters come from a microbenchmark on the target, run and frozen before the E3 matrix. | microbenchmark only |
| P3 | per-machine lookup table (S4-like) trained on the *training* families and sizes | yes (in-sample machine) |
| P3x | P3 trained on another machine (transfer) | no |
| P4 | empirical best per cell (oracle upper bound) | - |
| PR | uniformly random legal plan | no |

**Splits** (the overfitting controls): leave-one-family-out; held-out sizes (interpolation between grid points and extrapolation beyond them); leave-one-machine-out; leave-one-compiler-out. No predictor may see the timing of the cell it is scored on.

**Metrics.** Regret = time(chosen) / time(best), median, p95 and share above 1.05; decision accuracy on decisive cells (best beats second by >= 5 % in >= 75 % of paired units, as in E2); Kendall tau between predicted and measured plan order within a program.

**Gates** (proposed values):
- **H1 (a decision exists):** on at least two physical machines, in >= 20 % of programs the best legal plan beats P0 by >= 5 % decisively, reproduced in every replicate.
- **H2 (predictable without overfitting):** on held-out families, sizes and machines, P2 reaches p95 regret <= 1.05 and beats P3x.
- **Kill:** if H1 fails, the plan space is too flat for planning in this operation set; stop the planner thesis for element-wise add graphs.
- **If H1 holds but H2 fails** while in-sample P3 succeeds, the honest conclusion is per-machine empirical tuning (FFTW-style), not a predictive cost model.

**Controls.**
- A twin series for the noise floor.
- Latin-square order, untimed priming, replicates in fresh processes.
- An allocation regime fixed to `arena_warm` (E2 found regime effects weak).
- Two controlled buffer base offsets, since layout can flip conclusions.
- Recorded governor and boost state; cycles from the PMU where available, to separate frequency from work.
- A disassembly hash per plan, so that a code-generation change (for example a loop that fails to vectorize) is detected rather than attributed to memory.

**Confounders to address.**
- Different plans run different loop shapes, so compiler effects and memory effects are entangled. Mitigation: compare codegen reports per plan, and report the same verdict separately per compiler.
- Loop overhead at small N.
- Turbo and thermal state.
- THP and page placement.
- Aliasing at power-of-two strides.
- Microbenchmark parameters that are themselves noisy. Mitigation: freeze them, repeat them, and report their spread.

**Conditions that invalidate E3.**
- R1 INCONCLUSIVE.
- Noise floor at or above the 5 % effect size.
- Fewer than two eligible physical architectures.
- f32 plans that are not bit-identical (then only i32 is valid).
- PMU unavailable, in which case mechanism claims are dropped and only timing claims remain.

## 5. What E3 must not do

No planner, `CostModel`, `ExecutionPlan` or Synthesizer in core before E3's gates are met. The predictors above are experiment-local analysis code, deletable with the experiment. No claim of novelty beyond the measured result.

## 6. Decision

Proceed to a frozen E3 preregistration **only after** R1 has a non-INCONCLUSIVE verdict, choosing the branch from the table in section 2. Until then, the most valuable work is obtaining the R1 physical data.
