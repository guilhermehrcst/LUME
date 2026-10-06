# E2-R1 - Cross-machine physical replication of Materialize vs Recompute

> **Status: PREREGISTERED. No R1 physical benchmark data had been collected when this file was committed.**
> Part I is frozen at that commit. Results, deviations and the verdict go into Part II or `e2-r1-report.md`; any change to Part I is a separate, labelled amendment commit.

## Part I - Preregistration

### 1. Why R1

E2 ([report](e2-report.md), [post hoc audit](e2-audit.md)) measured one KVM guest. It found that the best Materialize-vs-Recompute action for a fixed (dtype, fan-out k) changes with N (a static quantity), reproduced on two compilers, but that a table keyed on (dtype, k, N) and constant across memory regimes stays within p95 1.006 / 1.005 of optimal (in-sample) and within p95 1.031 / 1.023 when the regime is held out (S4-CV). The generated code has no size-dependent path (see [codegen audit](e2-codegen-audit.md)), so the cause of the non-monotone N profile is unidentified. E2 therefore does **not** establish that a runtime, regime-sensitive planner is needed.

R1 asks, on physical hardware:

> **Does E2's size-dependent choice reproduce on physical machines, and is machine identity itself needed to choose the best static action?**

Three possibilities are to be separated:

1. **One portable static rule is enough.**
2. **Each machine needs static calibration** (per-machine table keyed on static facts).
3. **Even after machine, compiler, dtype, fan-out and N are known, the runtime memory regime changes the best choice meaningfully**, which would justify regime-sensitive planning.

R1 builds no planner, `CostModel`, `ExecutionPlan`, `verify_plan`, encoding or GPU backend, and adds no operation. It decides which architecture, if any, is justified.

### 2. Required hardware (a result from anything else is not R1 data)

- **Machine X**: physical Linux x86-64. **Machine A**: physical Linux ARM64 (aarch64).
- Not KVM or any other hypervisor guest, not a container, not a cloud VM, not a CI runner VM. A bare-metal cloud instance that reports no virtualization is acceptable if `check_physical_host.sh` passes.
- A hardware PMU exposed to the OS if possible (needed only for the secondary characterization, section 9).
- At least 8 GiB of RAM available and **no active swap** during the run (the largest cell holds about 1.7 GiB of buffers).
- 4 KiB base pages are expected (the workspace's page-touch stride is 4096 bytes); the page size is recorded and a different value is reported as a caveat, not as a failure.
- `check_physical_host.sh` is fail-closed: any positive virtualization or container signal (hypervisor CPU flag, `systemd-detect-virt` not `none`, DMI vendor/product naming a hypervisor, `/sys/hypervisor`, `.dockerenv` or container cgroups, device-tree hypervisor node) stops the primary run. A smoke run on a non-physical host is allowed only with an explicit environment override, writes into a `SMOKE-` bundle with `physical=false`, and the analysis refuses to treat such a bundle as physical.
- If this session's host is virtualized, **no R1 primary data is collected here**; R1 stops after tooling, unit and smoke tests.
- Recorded per host: exact CPU model, microarchitecture if known, cores/threads, cache hierarchy, RAM, kernel, glibc, compiler versions, CPU governor, turbo/boost state if observable, virtualization detection output, NUMA topology, THP, swap, page size, `perf_event_paranoid` and perf availability, git SHA and dirty state, build flags.

### 3. Matrix (E2's matrix, unchanged)

- dtype: f32, i32. Fan-out k: 1, 2, 3, 4, 6, 8. N: 64, 1 Ki, 4 Ki, 16 Ki, 64 Ki, 256 Ki, 1 Mi, 4 Mi, 16 Mi.
- Regimes: `arena_warm`, `arena_cold`, `glibc_default_fresh` (one process per cell). 324 cells per machine and compiler.
- Same M and R semantics (`ExecutionPolicy{reuse, no fusion, recompute off/on}`), same oracle, same protocol: 3 process replicates x 15 repetitions x {M, R, M_TWIN}, Latin-square order, untimed priming, K = 1000 calls per interval for N < 4096 (else 1), results held until the interval ends and checked against the oracle outside the timed region, no outlier removal. **The primary benchmark binary is `lume_e2_bench` from the merged E2 code, unmodified**, driven by the E2 run logic.
- Compilers: GCC Release `-O3 -DNDEBUG` and Clang Release `-O3 -DNDEBUG`, both where installed, no `-march=native`, no `-ffast-math`. Exact versions recorded. **Compiler results are never pooled.** A machine with only one compiler is reported as such.
- The harness refuses a scientific run from a dirty working tree and never overwrites an existing result directory.
- Expected samples: 43,740 per machine and compiler.

### 4. Primary static baseline S4 (fixed in advance)

For one machine and compiler, **S4** is one fixed action per (dtype, k, N), identical across the three memory regimes, chosen to minimize the mean log regret over that group's three regime cells (ties: M). It is an oracle-static baseline fitted to the same data (in-sample); this is intended: it hands the static side every advantage, so a gate that still fires measures real regime information. regret(cell, action) = median(time of action) / median(time of the empirically best strategy of the cell), medians over the pooled 45 samples.

Secondary, reported but not gating: **S4-CV** (the action for a cell is chosen from the other two regimes only).

### 5. Runtime-planning gates (per machine and compiler, independently)

- **A. Opportunity**: >= 20 % of the cells have S4 regret > 1.05 and the 2.5th percentile of a 1000-resample paired bootstrap (over the 45 paired units, seed 12345) of the median per-unit ratio exceeds 1.00 (as in E2).
- **B. Tail**: p95 (nearest rank) of S4 regret >= 1.15.
- **C. Pure regime flip**: for the same machine, compiler, dtype, k **and N**, two regimes have opposite *decisive* winners (winner's median >= 3 % lower and faster in >= 75 % of units), **each winner beats the alternative by >= 5 % in its own regime** (the wrong action costs >= 5 % in both affected regimes), and both decisive winners reproduce in each of the three process replicates analysed alone. Only pure regime flips count: a size flip does not, because N is static.

A gate "holds on a machine" only if it holds for **every installed compiler on that machine** (this guards against a compiler-specific artifact; E2 showed the two compilers disagree on about 10 % of winners). A gate that holds for one compiler only is reported prominently as an *unconfirmed regime signal* and does not by itself produce the SURVIVES verdict.

### 6. Cross-machine static transfer

For each compiler family present on both physical machines: **S4_X** (built from X) applied unchanged to A's cells, and **S4_A** applied to X's cells. Transfer regret of a cell = median(time of the transferred action) / median(time of the target cell's best strategy). The E2 VM's S4 tables (built from the committed E2 data, GCC and Clang) are also applied to both physical machines, as a secondary, non-gating comparison.

A transfer is a **machine-calibration failure** if either (a) >= 20 % of the target's cells have transferred regret > 1.05, or (b) the p95 transferred regret >= 1.10. If any gated transfer (X to A or A to X, any compiler family) fails, static policy is machine-specific. A passing transfer in both directions for every shared compiler family means the policy is portable. This does not imply a runtime planner.

### 7. Verdict (exactly one)

Evaluate in this order:

1. **INCONCLUSIVE** only for a real methodological failure: any correctness mismatch, an invalid cell (arena fallback, hook overflow, accounting mismatch), active swap, a non-physical host presented as physical, a twin-series noise p95 >= 1.10 on a machine/compiler, a required machine (X or A) missing, or an incomplete matrix. Losing the planner thesis is not a reason for INCONCLUSIVE.
2. **REGIME-SENSITIVE PLANNING SURVIVES** iff on at least one physical machine gate A, B or C holds (section 5, all installed compilers of that machine).
3. Otherwise **MACHINE-CALIBRATED STATIC POLICY** iff per-machine S4 gates all fail and at least one gated transfer fails (section 6).
4. Otherwise **PORTABLE STATIC POLICY SUFFICES** (all runtime gates fail and every gated transfer passes).

If gates A, B and C all fail on both machines the report states: *RUNTIME REGIME-SENSITIVE PLANNER FAILS FOR THIS SPACE.*

### 8. Secondary results (reported, never gating)

Per machine and compiler: winner map, M/R ratio tables, best global threshold, S3 and S1 regrets as in E2, S4-CV, k*, byte-model agreement, the machines' winner agreement per cell, and whether the non-monotone N profile of E2 (R catching up near 64 Ki and 4 Mi elements) appears.

### 9. PMU characterization (separate, secondary)

Fixed before any physical result: dtype f32 and i32; k = 1, 2, 3, 4; N = 4 Ki, 64 Ki, 1 Mi, 4 Mi; regime `arena_warm`; strategies M and R; both compilers. Counters are read around the timed interval of the same E2 protocol by a **separate binary** (`lume_e2r1_pmu`) and written to a separate CSV; **perf-instrumented timing is never mixed into the primary dataset**.

Events are probed (not assumed) through the architecture-independent generic `perf_event_open` encodings: hardware cycles, instructions, branches, branch-misses, cache-references, cache-misses; hardware-cache L1D read accesses and misses, LL read accesses and misses, dTLB read misses; software page-faults, minor-faults, major-faults. Unsupported events are recorded as unsupported. To avoid multiplexing, the events are measured in three passes of at most four hardware events each (A: cycles, instructions, branches, branch-misses; B: cache-references, cache-misses, L1D accesses, L1D misses; C: LL accesses, LL misses, dTLB misses, plus the software events), and `time_enabled`/`time_running` are recorded so any multiplexing is visible. This subset yields mechanism evidence only; causal claims need an experiment, not a correlation.

### 10. Code generation on the physical machines (secondary)

`codegen_report.sh` is run for GCC and Clang on both machines (4 reports): SIMD width, vector loads/stores/adds per iteration, unrolling, tail handling, alias/alignment paths, size thresholds. The reports are compared to separate "memory hierarchy" from "compiler/kernel shape"; no causality is claimed without evidence.

### 11. Predictions recorded before any physical data (so they can be wrong)

1. The M/R winner depends on N on both machines (a size-dependent static choice replicates qualitatively), because the extra pass of M costs less when everything is cache-resident.
2. The N positions where the winner changes differ between X and A (different cache sizes and line behaviour), so a transferred table fails the criterion in section 6 on at least one direction.
3. Pure regime flips (section 5, gate C) are rare or absent on physical machines; the E2 `glibc_default_fresh` effect at N = 64 Ki / 4 Mi may or may not reproduce.
4. The most likely verdict is MACHINE-CALIBRATED STATIC POLICY. My confidence is moderate, not high; REGIME-SENSITIVE PLANNING SURVIVES is possible if the glibc effect is real.

### 12. Threats known in advance

Two machines are two data points (no claim about other CPUs); desktop/laptop hosts have turbo, thermal and governor variance; ARM64 kernels may use 64 KiB pages; arena regimes use a replaced `operator new` pool, not an in-runtime arena; the primary benchmark has no CPU pinning (as in E2); the S4 baselines are oracle-static and optimistic; the ARM64 build and run paths of the harness are **untested before the first ARM64 run** (only parsing and syntax fixtures were exercised on x86); results from a single host per architecture cannot separate "this architecture" from "this machine".

### 13. Procedure and deliverables

`experiments/e2-r1/run_replication.sh <machine-id>` is the single entry point (see its `README.md`). It writes to `docs/experiments/e2-r1-results/<machine-id>/` only after the whole run validates, never overwrites, records and checksums everything, and emits a tarball with a SHA-256 for transport. Machine data are committed only when complete and validated. Cross-machine analysis: `experiments/e2-r1/analyze_cross_machine.py`.

---

## Part II - Results

*(filled in after physical data exist; see `e2-r1-report.md` and `e2-r1-results/`)*
