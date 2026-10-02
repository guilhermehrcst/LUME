# Lume M7: Single-Use Intermediate Fusion

**Status:** runtime optimization experiment. Branch `lume/m7-single-use-intermediate-fusion-v01`, base `main` = `f636bdff134527165d08f3aea191ae031944aea3` (M0–M6, Experiment 002, rename to Lume). Recommendation: see §31.

Labels: **MEASURED** was observed here; **INFERRED** is reasoning from measurements; **FALSIFIED** was contradicted; **NOT YET KNOWN** was not established.

## 1. Research question

Can Lume execute two adjacent `add` operations whose intermediate has no other use as **one loop**, without a buffer for the intermediate, while keeping the IR, the verifier, the result semantics and **M6's allocation count** unchanged?

M5 measured two costs inside the intermediate `D` of `D = A + B; E = D + C`: its storage lifetime (allocation, trim, re-fault) and its data movement (write D, read D). M6 removed the first. M7 removes only the second:

| | M6 | M7 |
| --- | --- | --- |
| result allocations (final-only chain) | 1 | 1 |
| logical peak result storage | 4 B/element | 4 B/element |
| computational passes | 2 | 1 |
| user-level payload model | 24 B/element | 16 B/element |

## 2. M5/M6 motivation

- M5: the final-only chain is a two-pass computation with a 24 B/element payload; the native fused loop `e[i] = (a[i] + b[i]) + c[i]` is 16 B/element.
- M6: computing E in D's storage removed the second result allocation and, under default glibc, its trim/re-fault cycle (2,016 → 0 faults per call at N = 1M). The chain still writes D and reads it back.
- So after M6 the only known remaining difference between the Lume chain and the native fused loop is the materialized intermediate.

## 3. Pre-registered hypotheses

From the M7 specification, fixed before any M7 measurement:

| # | Hypothesis | Outcome |
| --- | --- | --- |
| H1 (T1) | M7 final-only approaches native fused; naive model M7/M6 = 16/24 = 0.667, plausible 0.67–0.85. Not a pass criterion. | **Supported:** M7 = native fused (1.000 at 1M, 1.007 at 4M); M7/M6 = 0.815 / 0.785. The 0.667 point prediction is falsified (§22). |
| H2 (default) | M7 keeps one allocation, so any default-glibc speed-up cannot come from allocation. | **Supported:** 1 = 1 allocation, 0 = 0 faults; M7/M6 = 0.811 / 0.817. |
| H3 (T2) | Fault count unchanged (≈ 1,025 per call at 1M). A material fall means M7 changed more than fusion. | **Supported exactly:** 1,025 = 1,025 (1M), 4,097 = 4,097 (4M). |
| H4 | Single add unchanged (any regression is detector overhead). | **Supported:** within the noise floor at every N. |
| H5 | Intermediate-output chain unchanged (not fused). | **Supported.** |
| H6 | Output-after-use chain unchanged (not fused; D observable). | **Supported.** |
| Alloc | Primary workload: one result allocation in M6 and in M7. | **Supported** (black-box count). |
| Peak | Logical peak result storage 4 B/element in both. | **Supported** (by construction, §21). |

## 4. Strict eligibility rule

Let operation `k` be `T = add(A, B)` and operation `k + 1` the next operation. The pair is fused only if **all** hold (`src/runtime/cpu_reference.cpp`, lambda `fusible`):

1. operation `k + 1` exists and is an `add` (adjacent; the detector never searches forward or skips anything);
2. `T` is exactly one operand of operation `k + 1` (`T + T` is rejected);
3. `last_use[T] == k + 1`: no later computation and no output reads `T` (the M2 `last_use` table counts outputs as uses);
4. neither `A` nor `B` is M6-reusable at `k` (`owned[v] && last_use[v] == k`);
5. the other operand `C` of operation `k + 1` is not M6-reusable at `k + 1`;
6. `T` is `i32`, or `f32` on a toolchain where float expressions carry no excess precision (`FLT_EVAL_METHOD == 0`; undefined counts as "no").

The M6 rule is the same lambda (`reusable(v, at)`) for the M6 path and for conditions 4–5: one source of truth. Condition 5 is evaluated at `k`: by condition 4, M6's operation `k` would reuse neither `A` nor `B`, so it changes no `owned` slot other than `T`'s, and by condition 2, `C` is not `T`.

Greedy and deterministic: operations are scanned in order; a fused pair consumes operations `k` and `k + 1`, and scanning resumes at `k + 2`. Operation `k + 1` is never the first add of another pair.

## 5. Why M6-reusable sources are excluded

M7 must change one variable. If the first add could already run in place under M6 (condition 4), M6 allocates nothing for the whole pair (it computes `T` in `A`'s storage, then `R` in `T`'s), while fusion would allocate `R`: one allocation more. If `C` is M6-reusable at `k + 1` (condition 5), M6 would compute `R` in `C`'s storage (for `R = C + T`, lhs preference) or keep the pair's storage mapping different from the fused one. With 4 and 5:

```text
M6:  op k   allocate T, compute A + B            (A, B not reusable)
     op k+1 compute R in T's storage, T -> R      (T reusable; C is not)
M7:  ops k, k+1  allocate R, compute (A + B) + C or C + (A + B) in one loop
```

Both perform exactly one result allocation, and the executor state after the pair is identical: `owned[R]` holds one buffer, `owned[T]` is empty, `bound[T]` is null, every other slot is unchanged. Only the physical buffer's history differs (written twice in M6, once in M7).

## 6. Semantic model

- The IR still contains `T` and both adds. The verifier and `VerifiedProgram` are unchanged; no `FusedAdd`/`Add3` opcode exists. Fusion is a decision of the executor over verified IR.
- `T` is unobservable when fused: condition 3 excludes every later read and every output of `T`, and condition 1 excludes any operation between the two adds.
- Per element, the fused loop computes `t = add(a[i], b[i])` and then `add(t, c[i])` or `add(c[i], t)` in IR operand order: `wrap(wrap(A + B) + C)` for `i32`; `(A + B) + C` or `C + (A + B)` with binary32 rounding of `t` for `f32`.
- Outputs, output order, copy/move behavior, input validation and error codes are unchanged. Caller inputs are read only; the fused loop writes only a fresh `OwnedArray` that becomes `R`.
- Fail closed: the detector reads state only. Before any allocation the fused path checks that the other operand is bound and that all three buffers agree in scalar type and length; otherwise it returns `internal_invariant_violation` (a fresh `ExecutionResult` with no outputs), exactly as the M6 path does for its own checks. `R` is bound only after its buffer is complete.

## 7. Floating-point policy

- **Grouping is preserved.** The source keeps `t = a + b` as a separate, named value; neither compiler reassociates IEEE additions without `-ffast-math`/`-fassociative-math` (not used). The integrated code (§23–24) adds `a` and `b` first, then `c`.
- **Rounding of `t`.** Under `FLT_EVAL_METHOD == 0` a `float` expression is evaluated in `float`, so the register value of `t` equals the binary32 sum that M6 stores in memory. Where excess precision is possible (x87) or the macro is missing, f32 pairs are not fused (condition 6); i32 is unaffected. On the measured toolchains (GCC 13.3, Clang 18.1, x86-64) the macro is 0. A forced "not fused" build passes the full suite (`docs/data/m7/mutations/guard_off_probe.txt`).
- **Contraction.** There is no multiplication, so FMA contraction cannot occur.
- **Non-NaN results** must be bit-identical to the oracle (the oracle uses a materialized intermediate).
- **NaN results** follow the existing policy: any NaN matches any NaN. Source operand order is not a machine-level guarantee: GCC canonicalizes a commutative add (in Phase A its identical-code folding merged the right-consumer kernels into the left ones), and Clang emits the same instruction order for both consumer sides. So NaN payloads may differ from M6; they were never portable across compilers (M6 §7).
- **Audit (MEASURED;** `nan_audit.cpp`, `nan_audit_results.txt`): both fusible shapes, N = 65,536, inputs with about 1/8 NaNs of random sign and payload (quiet and signaling) and 1/32 infinities, executed by the M6 and the M7 library. Non-NaN bit differences: **0** in every comparison; NaN-vs-non-NaN differences: **0**. NaN payload differences, M6 vs M7, of 21,683 NaN results: GCC 980 (left) and 2,764 (right); Clang 0 and 1,911. For scale, M6 itself differs between GCC and Clang in 1,911 payloads.

## 8. Phase A

Isolated kernels (`docs/data/m7/phase_a/fused_kernel.cpp`, independent of the runtime; expected values from the oracle with a materialized intermediate):

```cpp
// f32 left consumer             // f32 right consumer
const float t = a[i] + b[i];     const float t = a[i] + b[i];
r[i] = t + c[i];                 r[i] = c[i] + t;
// i32: wrapping_add(a[i], b[i]) then wrapping_add(t, c[i]) / wrapping_add(c[i], t)
```

- **Correctness (MEASURED, 67 checks):** N ∈ {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099}, random finite f32 and random i32; f32 signed zero, ±inf, `inf + -inf`, NaNs with distinct payloads (quiet and signaling, both signs), subnormals, overflow to infinity; i32 `INT32_MIN`, `INT32_MAX`, −1, 0, 1, overflow, underflow and chained wrapping (also checked against hand-computed values; the first hand-computed value for one element was wrong, and the oracle-based check exposed the error in the expectation, not in the kernel).
- **Rounding-order sentinels:** `(1, 2^-24, 2^-24)` and `(2^-24, 2^-24, 1)` give different results under `(A+B)+C`, `A+(B+C)` and `(C+A)+B`. A rounding-sensitive random set (random sign, 24-bit significand, exponent in [−20, 20]) differs under reassociation in 615 (left) and 639 (right) of 4,099 elements. The oracle's own generator (multiples of 2^-23 in [−1, 1)) almost always sums exactly and is blind to grouping, which is why both were added.
- **Reassociation mutation** (`a + (b + c)`; `(c + a) + b`): killed by both compilers (4 failures each: the sentinels, the rounding-sensitive set and the specials).
- **Sanitizers:** GCC ASan + UBSan at `-O1` and `-O3`: clean. Clang's sanitizer runtime (compiler-rt) is not installed in this environment (known since M0).
- **Vectorization (MEASURED, `-O3`, no `-march`, no fast-math):** GCC 13.3: all four loops vectorized with 16-byte vectors (plus an 8-byte epilogue), versioned for possible aliasing; with default flags GCC's identical-code folding merges `fused_right_*` into `fused_left_*`, so the right kernels are reported only with `-fno-ipa-icf`. Clang 18.1: all four "vectorization width: 4, interleaved count: 2". Hot loop: `movups A; movups B; movups C; addps; addps; movups R` (f32) and the same with `movdqu`/`paddd` (i32). No calls in any kernel.
- **Gate:** passed → runtime integration.

## 9. Runtime implementation

All production changes are in `src/runtime/cpu_reference.cpp`, plus a comment in `include/lume/runtime/cpu_reference.hpp` and one new internal header:

- `add_add<T>`: the fused kernel, one loop per consumer side, `const T t = add(a[i], b[i]); r[i] = add(t, c[i])` (or `add(c[i], t)`), writing a fresh `OwnedArray<T>::for_overwrite(n)`; every element is written exactly once (M4's single-write invariant).
- `add_add_buffers`: checks the three views (scalar type, length) **before** allocating; `nullopt` on disagreement.
- `reusable(v, at)`: M6's reuse rule, hoisted out of the add case so the M6 path and the M7 detector share it. The M6 path's behavior is unchanged.
- `fusible(k)`: the detector of §4. It only reads `s.operations`, `last_use`, `owned` and the value's type.
- In the add case, before the M6 path: if `fusible(i)`, look up the other operand, run `add_add_buffers`, set `owned[R]`/`bound[R]`, skip operation `i + 1` (`++i`), done. Otherwise the M6 path runs exactly as before.
- `detail::execute_cpu_reference_observed(program, inputs, fused)` in `src/runtime/fusion_observer.hpp` (internal, not installed): the same executor, which appends the first operation index of every fused pair to `*fused` when the pointer is non-null. `execute_cpu_reference` calls it with `nullptr`. It exists because fusion is unobservable through the public API by design (same values, same allocation count), and three negative controls and one mutation (§12, §13) can only be checked by seeing the decision.

## 10. Ownership / binding model

| After the pair | M6 | M7 |
| --- | --- | --- |
| `owned[R]` | the buffer allocated at `k` (T's, reused) | the buffer allocated by the fused op |
| `bound[R]` | `&*owned[R]` | `&*owned[R]` |
| `owned[T]`, `bound[T]` | empty, null (moved to R) | empty, null (never set) |
| `A`, `B`, `C` | untouched | untouched |
| result-sized allocations | 1 | 1 |

`T` is never allocated, written, bound or owned on the fused path. There is no window in which two ValueIds own one buffer, and no stale pointer: nothing is moved.

## 11. Positive cases

`tests/execution/fusion_test.cpp` checks values against the oracle, that caller inputs are unchanged, the number of result-sized allocations per call (global `operator new` counting, as in M6's test), and the fused-pair list from the observation seam:

| Case | Program | fused | allocations |
| --- | --- | --- | --- |
| left consumer | `D = A + B; E = D + C; out E` | yes | 1 |
| right consumer | `D = A + B; E = C + D; out E` | yes | 1 |
| same input everywhere | `D = A + A; E = D + A` | yes | 1 |
| fused result reused later | `D = A + B; E = D + C; out E; F = E + G; out F` | (D, E) | 2 (E, copy of E; F reuses E) |
| 3-add chain | `V1 = A + B; V2 = V1 + C; V3 = V2 + F` | (V1, V2) only | 1 |
| 4-add chain | … `; V4 = V3 + G` | (V1, V2) only | 1 |
| repeated execution | same VerifiedProgram × 4 | same each time | 1 each; identical checksums |
| rounding sentinels | both sides, N = 2 | yes | – |
| edge lengths | N ∈ {1 … 4099}, both sides | yes | – |
| f32 specials | −0, ±inf, NaN payloads, sNaN, subnormals, overflow | yes | – |
| i32 | wrap boundaries both sides; random | yes | 1 |

The program-shape cases use the rounding-sensitive f32 set (§8), so a grouping change is visible in them; the edge-length cases use the oracle's generator (insensitive to grouping) and the sentinels cover grouping at N = 2.

## 12. Negative controls

| Control | Program | Why blocked | fused | allocations (= M6) |
| --- | --- | --- | --- | --- |
| intermediate output | `D = A + B; out D; E = D + C; out E` | not adjacent (cond. 1), D observable | no | 2 |
| later output | `D = A + B; E = D + C; out D; out E` | `last_use[D]` is the output (cond. 3) | no | 2 |
| non-adjacent | `D = A + B; X = C + F; E = D + G; out E; out X` | consumer is not the next op (cond. 1) | no | 2 |
| repeated operand | `D = A + B; E = D + D` | T appears twice (cond. 2) | no | 1 |
| first-add M6-reusable | `X = A + B; Z = C + F; T = X + C; R = T + G` | X reusable at T (cond. 4) | no | 2 (fusing would make 3) |
| second-other M6-reusable | `X = A + B; Z = C + F; T = A + C; R = T + X` and `R = X + T` | X reusable at R (cond. 5) | no | 3 |
| i32 later output, i32 `T + T` | as above, i32 | cond. 3, cond. 2 | no | 2, 1 |

M6's own test (`inplace_reuse_test`, unchanged) also still passes; several of its programs are now fused (final-only, right operand, longer chain, i32), and every allocation count it asserts is unchanged.

## 13. Mutation tests

Each mutation was applied to `src/runtime/cpu_reference.cpp`, built in a separate tree (build success checked, so no stale binary), run, and reverted; none is committed. Script: `docs/data/m7/mutations/mutate.py`; outputs `mutation_*.txt`.

| Mutation | What it does | Killed by |
| --- | --- | --- |
| A — reassociation | fused loop computes `a + (b + c)` / `(c + a) + b` | 18 value failures in `fusion_test` (sentinels, rounding-sensitive data, checksums); `chain_test` (M5 evaluation-order test) also fails |
| B — ignore last use | condition 3 removed | fails closed: `out D` after a fused pair finds D unbound → `internal_invariant_violation` (later-output controls, f32 and i32); `output_move_test`, `chain_test`, `inplace_reuse_test` and the benchmark smoke test also fail |
| C1 — skip adjacency (index bug) | detector searches forward for T's consumer; executor still skips `i + 1` | fails closed in the non-adjacent, first-add and second-other controls |
| C2 — skip adjacency (careful) | as C1, but the consumer is marked done and skipped correctly | values and allocation counts are **correct**; killed only by the observation seam (`fused.empty()` fails in the non-adjacent and first-add controls) and by an execution failure in the second-other control |
| guard off (not a mutation) | f32 fusion forced off (condition 6 false) | nothing fails: 23/23, f32 pairs run through M6 with the same values and allocation counts (`guard_off_probe.txt`) |

C2 is the reason for the observation seam: a detector that crosses an independent operation can produce correct values and M6's allocation count, so black-box tests cannot see it.

## 14. Benchmark methodology

- **Primary harness:** the unmodified M5 benchmark `lume_bench_intermediate_materialization` (byte-identical to `main`). Its `lume_*` components follow each tree's runtime; its native components are identical code in both trees and serve as the noise floor.
- **Trees:** A = canonical `main` `f636bdf` (M6 runtime) in a clean worktree; B = this branch. Both built with the same flags (CMake `Release`: `-O3 -DNDEBUG`, no `-march`, no fast-math), GCC 13.3 and Clang 18.1.3. B's `cpu_reference.o` was rebuilt after the last code change (the `FLT_EVAL_METHOD` guard) and its disassembly is identical to the measured one for both compilers.
- **Harness inside the benchmark (unchanged since M1):** 5 warmup, 51 timed iterations, median; K = 1000 repetitions per interval below N = 4,096; seed 42; faults from `getrusage`; every result checked against the oracle outside the timed region.
- **Order:** A and B alternate within each pair, and the first side alternates between pairs (A-B, B-A, …).
- **Broad matrix (all six N = 1 … 4,194,304):** GCC default 6 pairs, reverse component order 3 pairs, T1 3, T2 2; Clang default 3, T1 2; small N (N = 1, 256) 10 pairs per compiler. Raw: `docs/data/m7/{A,B}_*.txt`; tables: `summary_tables.txt` (`aggregate.py`), `broad_paired_analysis.txt`, `smalln_analysis.txt`.
- **Focused matrix (N = 65,536, 1,048,576, 4,194,304):** GCC default 20 pairs, T1 20, T2 10; Clang default 10, T1 10. Raw: `docs/data/m7/focused/`; analysis: `focused_paired_analysis.txt` (`paired_analysis.py`).
- **Why the focused matrix:** single runs on this shared VM drift by about ±10 % even for identical code (native loops, broad matrix: B/A between 0.84 and 1.23). Conclusions therefore use **paired** ratios (adjacent runs), ratios **normalized** to each run's own native fused loop, and a sign test over pairs.
- T1 = `GLIBC_TUNABLES=glibc.malloc.trim_threshold=1073741824:glibc.malloc.mmap_threshold=33554432`; T2 = `glibc.malloc.mmap_threshold=131072`.
- Machine: shared 4-vCPU VM (Intel Xeon @ 2.10 GHz, L2 2 MiB per core, L3 260 MiB), Linux 6.18, glibc 2.39. This is a different VM instance from the one M6 was measured on, so only the same-session A/B is compared, never M6's published absolute numbers.
- Every result line in every raw file reports `correctness=exact`; no `MISMATCH` anywhere. The benchmark's `model_bytes`/`model_gbps` columns for `lume_chain_final_only` still use M5's 24 B/element model in both trees (the benchmark was not changed); for B the true model is 16 B/element.

## 15. Allocation-count evidence

- **Primary workload, black box:** `fusion_test` counts result-sized `operator new` calls per execution: left consumer 1, right consumer 1. M6's own `inplace_reuse_test` (unchanged, still passing) asserts the same programs allocate 1 under M6, and those programs are fused under M7.
- **Every shape:** the counts in §11–12 are the M6 counts, for fused and non-fused programs alike.
- **Under the allocator:** T2 fault sets (§18), the per-call faults of the strace probe and the `brk`/`mmap`/`munmap` sequences (§20) are identical between A and B for every shape.

## 16. Default A/B

Focused matrix, GCC, default glibc (medians of per-run medians; paired = median of per-pair B/A; normalized = median of per-pair ratios after dividing each run by its own native fused loop):

| N | M6 final-only | M7 final-only | paired M7/M6 | normalized | pairs with M7 faster | faults/call M6 → M7 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 65,536 | 16.7 µs | 13.7 µs | 0.846 | 0.945 | 14/20 (p = 0.12) | 0 → 0 |
| 1,048,576 | 817 µs | 666 µs | **0.811** | **0.820** | **20/20** (p = 2e-6) | 0 → 0 |
| 4,194,304 | 3,232 µs | 2,600 µs | **0.817** | **0.801** | **20/20** (p = 2e-6) | 0 → 0 |

Controls in the same runs (paired M7/M6; identical-code native loops in brackets as noise floor):

| N | single add | intermediate output | output after use | [native fused] | [native two-pass] |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 65,536 | 1.008 | 0.997 | 1.008 | [0.911] | [1.031] |
| 1,048,576 | 1.021 | 1.007 | 1.023 | [1.011] | [1.018] |
| 4,194,304 | 1.002 | 1.048 | 1.063 | [1.015] | [1.046] |

Broad matrix, all N, final-only normalized M7/M6 (GCC forward 6 pairs / GCC reverse 3 / Clang 3): N = 4,096: 0.879 / 0.881 / 0.837 (all pairs faster); 65,536: 0.928 / 0.710 / 0.798; 1M: 0.761 / 0.771 / 0.800; 4M: 0.761 / 0.855 / 0.724. At N = 1 and 256 the native fused loop takes 1–40 ns, too short to normalize by; small N is in §26.

The paired saving (M6 time − M7 time) at 1M is **152 µs** (IQR 117–176), at 4M **574 µs** (IQR 425–737).

## 17. T1

Focused matrix, GCC, 20 pairs:

| N | native fused | native two-pass | M6 final-only | M7 final-only | paired M7/M6 | M6 / native fused | **M7 / native fused** |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,048,576 | 659–669 µs | 974–975 µs | 810 µs | 649 µs | 0.815 (20/20) | 1.230 | **1.000** |
| 4,194,304 | 2,576–2,615 µs | 3,941–3,943 µs | 3,288 µs | 2,584 µs | 0.785 (20/20) | 1.246 | **1.007** |

Paired saving: 143 µs at 1M, 710 µs at 4M. Controls: single add 0.989 / 0.996, intermediate output 0.973 / 0.948, output after use 0.976 / 0.985 (1M / 4M). At 65,536: final-only 0.869 paired, 12/20 pairs, not significant.

Clang (10 pairs each): default 0.815 (1M) / 0.813 (4M), T1 0.779 / 0.803, all 10/10 pairs; M7 / native fused 0.98–1.03; paired saving 150 / 592 µs (default) and 183 / 599 µs (T1).

## 18. T2

Forced mmap (every result allocation is a fresh mapping), GCC, 10 pairs:

| N | M6 final-only | M7 final-only | paired M7/M6 | faults/call M6 → M7 | paired saving |
| ---: | ---: | ---: | ---: | --- | ---: |
| 65,536 | 70.6 µs | 67.1 µs | 0.976 (6/10) | 65 → 65 | 1.7 µs |
| 1,048,576 | 1,692 µs | 1,547 µs | 0.919 (7/10, p = 0.34) | **1,025 → 1,025** | 141 µs |
| 4,194,304 | 6,885 µs | 5,999 µs | 0.908 (10/10, p = 0.002) | **4,097 → 4,097** | 586 µs |

Fault counts of the controls are also unchanged (single add 1,025; intermediate output and output after use 2,050 at 1M). T2 calls spend roughly 1 ms per 1M-element result on page faults, which fusion does not change, so the same absolute saving is a smaller fraction and noisier (IQR at 1M: −46 to 386 µs).

## 19. Page faults

| Program (N = 1M) | default M6 → M7 | T1 | T2 M6 → M7 |
| --- | --- | --- | --- |
| single add | 0 → 0 | 0 → 0 | 1,025 → 1,025 |
| final-only (fused in M7) | 0 → 0 | 0 → 0 | 1,025 → 1,025 |
| intermediate output | 2,016 → 2,016 | 0 → 0 | 2,050 → 2,050 |
| output after use | 2,016 → 2,016 | 0 → 0 | 2,050 → 2,050 |

At 4M the same with 8,160 (default) and 4,097 / 8,194 (T2). **No fault count changed anywhere.** The default-glibc trim/re-fault cycle of the two-result controls (M5's finding) is untouched, as intended.

## 20. strace

`m7_probe.cpp`, 10 calls at N = 1M, `brk`, `mmap`, `munmap`, `madvise` (`strace_summary.txt`, raw traces in `strace/`). For **all five shapes** (left- and right-consumer final-only, intermediate output, output after use, single add) under **both** default and T2, A and B have identical syscall counts, identical `brk` delta sequences (3–29 deltas) and the same faults per call (within 0.1). Under T2 both perform one 4 MiB mmap/munmap pair per call for the fusible shapes and two for the controls; under default glibc the controls show M6's +4 MiB / +4 MiB / −8 MiB `brk` cycle on every call in both trees, and the fusible shapes show none. The speed-up is therefore not an allocator or lifetime effect.

## 21. Logical peak payload

Model (bytes per element of executor-owned result storage alive at once), not RSS:

| Program | M6 | M7 |
| --- | ---: | ---: |
| final-only (left or right) | 4 | **4** |
| intermediate output | 8 | 8 |
| output after use | 8 | 8 |
| single add | 4 | 4 |

Unchanged by construction: the fused path allocates exactly the buffer M6 would have allocated at operation `k`, and never a second one.

## 22. Data-movement model

User-level payload of the final-only chain, minimum bytes per element:

| | pass 1 | pass 2 | total |
| --- | --- | --- | ---: |
| M6 | read A, B; write D (12) | read D, C; write D (12) | 24 |
| M7 | read A, B, C; write R (16) | — | **16** |
| native fused | read A, B, C; write E (16) | — | 16 |

The naive ratio is 16/24 = 0.667; measured M7/M6 is 0.78–0.82, and M7 equals the native fused loop (0.98–1.03). **INFERRED:** the 24 B/element model overstates M6's cost. M6's second pass writes back into lines it has just read (M6 §14 measured that in-place pass at about 0.84× an out-of-place one), and M6 here ran at 1.23–1.25× native fused, not 1.5×. Eliminating the intermediate removed exactly that remaining 23–25 %. All working sets here (up to 5 × 16 MiB) fit in this VM's 260 MiB L3, so the model is not a DRAM-traffic model, and no counter measured actual bytes.

Evidence that the intermediate is not materialized, from independent sources: (1) source: `t` is a loop-local value and the fused path never allocates, binds or owns `T`; (2) allocation counts: one result-sized allocation, same as M6 (§15); (3) codegen: per vector iteration three loads, two adds, one store and nothing else (§23–24); (4) time: equal to the native fused loop that has no intermediate by construction (§17); (5) faults and syscalls identical to M6 (§19–20), so the time difference is not allocator behavior. No hardware counters were available (§25).

## 23. GCC codegen

Integrated executor (`src/runtime/cpu_reference.cpp` as shipped, `-O3 -DNDEBUG`; `integrated_vectorization_reports.txt`): both fused loops (left and right consumer) "loop vectorized using 16 byte vectors", with an 8-byte epilogue, "versioned for vectorization because of possible aliasing" (a runtime check that R does not overlap A, B or C; R is always a fresh buffer, so the vector path is taken). Per-type attribution with a probe that only adds `noinline` to `add_add` (`disasm_integrated_probe_g++.txt`, `integrated_codegen_summary.txt`):

```text
f32 (each consumer side)          i32 (each consumer side)
movups (A),%xmm0                  movdqu (A),%xmm0
movups (B),%xmm4                  movdqu (B),%xmm4
movups (C),%xmm5                  movdqu (C),%xmm5
addps  %xmm4,%xmm0                paddd  %xmm4,%xmm0
addps  %xmm5,%xmm0                paddd  %xmm5,%xmm0
movups %xmm0,(R)                  movups %xmm0,(R)
```

The only calls in the instantiations are `operator new[]` (the single result allocation, before the loop) and a cold `__cxa_throw_bad_array_new_length`; none in any loop.

## 24. Clang codegen

Clang 18.1.3, same file and flags: both fused loops "vectorized loop (vectorization width: 4, interleaved count: 2)". Per iteration of the f32 loop: six `movups` loads (A, B, C × 2), four `addps` (`a + b` first, then the sum with `c`), two `movups` stores; i32 the same with `movdqu`/`paddd`. Calls: `operator new[]` only. Clang emits the same instruction sequence for both consumer sides (the IEEE add commutes), so the source operand order is not preserved at machine level (§7).

## 25. Optional hardware counters

Not available. `perf_event_open` returns `ENOENT` for every hardware event (cycles, instructions, cache references, cache misses): the VM exposes no PMU. `perf` is not installed, and nothing was installed or reconfigured (`hardware_counters.txt`). Only the software page-fault counter exists, and it agrees with `getrusage`.

## 26. Small N

10 paired runs per compiler (`smalln_analysis.txt`; median paired difference M7 − M6, and in how many pairs M7 was slower):

| N | Program | GCC | Clang |
| ---: | --- | --- | --- |
| 1 | single add | +0.3 ns (6/10) | +3.1 ns (6/10) |
| 1 | final-only (fused) | −10.9 ns (2/10) | +2.3 ns (5/10) |
| 1 | intermediate output | −2.0 ns (4/10) | −3.2 ns (4/10) |
| 1 | output after use | −6.9 ns (5/10) | −4.1 ns (4/10) |
| 256 | single add | +7.3 ns (7/10) | +2.3 ns (5/10) |
| 256 | final-only (fused) | −28.7 ns (2/10) | −31.8 ns (3/10) |
| 256 | intermediate output | +16.2 ns (7/10) | −9.8 ns (3/10) |
| 256 | output after use | +21.8 ns (7/10) | −1.2 ns (5/10) |

No difference for a non-fusible program reaches a sign-test p below 0.34; the largest (GCC, N = 256, output after use: +22 ns, ≈ 6 %) is within the spread of the identical native loops in the same runs. **The detector cost is not detectable at this resolution.** It is bounded by a few comparisons per add: one opcode check, two id comparisons and at most three `owned`/`last_use` lookups before the M6 path.

## 27. Longer-chain control

Correctness (tests): `V1 = A + B; V2 = V1 + C; V3 = V2 + F` fuses exactly the pair (V1, V2); V3 then runs as M6's in-place add on V2, because V2 is M6-reusable at V3, which condition 4 rejects for the pair (V3, V4). The 4-add chain behaves the same: one fused pair, then two in-place adds. One result allocation in both, as in M6.

Supplementary measurement (`longer_chain/summary.txt`, `m7_probe.cpp`, N = 1M, 51 calls per run, 10 interleaved pairs, with a native single loop of the same grouping measured in the same process):

| Chain | config | M6 | M7 | paired M7/M6 | M6 / native | M7 / native |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 3 adds | default | 1,170 µs | 988 µs | 0.854 (8/10) | 1.44 | 1.26 |
| 3 adds | T1 | 1,157 µs | 971 µs | 0.850 (10/10) | 1.45 | 1.24 |
| 4 adds | default | 1,518 µs | 1,357 µs | 0.859 (9/10) | 1.54 | 1.39 |
| 4 adds | T1 | 1,369 µs | 1,244 µs | 0.899 (8/10) | 1.51 | 1.35 |

Faults per call are equal in A and B. Pairwise fusion removes one intermediate per chain; each later add is still a separate in-place pass, so longer chains stay 1.24–1.39× from a single loop.

## 28. Threats to validity

- **One machine, shared VM.** One VM instance (different from M6's), with run-to-run drift of about ±10 % even for identical code. Mitigated by paired, order-alternating runs, normalization to each run's native fused loop, 20-pair focused sets at the sizes that carry the hypotheses, and sign tests. At N = 65,536 the result is not significant (12–14 of 20 pairs).
- **Cache residency.** The L3 here is 260 MiB, so every working set (up to ≈ 80 MiB at 4M) is L3-resident; "bytes per element" is a payload model, not measured DRAM traffic. On a machine where these sizes reach DRAM, the ratio may differ.
- **No hardware counters.** Cycles, instructions and cache traffic were not measured (§25).
- **Allocator dependence.** glibc 2.39 only. Faults and syscalls were equal between A and B, so allocator behavior cannot explain the difference here, but other allocators were not measured.
- **Compiler dependence.** GCC 13.3 and Clang 18.1, x86-64 SSE2 baseline only. MSVC, Apple Clang and AArch64 were not built locally. Whether MSVC's `<cfloat>` defines `FLT_EVAL_METHOD` is NOT YET KNOWN: if it does not, f32 pairs are not fused there (correct, slower); `fusion_test` prints which mode ran.
- **Code layout.** A and B are different binaries. The identical native loops in each run are the control for layout and drift; they agree within noise in the focused matrix (paired B/A medians at 1M and 4M between 0.97 and 1.05).
- **Timer and batching.** Small-N intervals use K = 1000 repetitions; the detector's cost is below this resolution (§26).
- **NaN payloads.** M7 changes NaN payloads relative to M6 for some elements (§7); non-NaN results are bit-identical. Any consumer relying on NaN payloads was already not portable between GCC and Clang.
- **Source vs machine operand order.** The source keeps IR operand order; compilers commute the addition (GCC folds the two consumer sides into one function in Phase A; Clang emits one sequence for both). Grouping, not operand order, is what is guaranteed.
- **Scope of the IR.** Only `input`, `add`, `output`; only adjacent pairs; only f32/i32. Conclusions do not transfer to other operations without new evidence.
- **Observation seam.** Tests use an internal entry point (`execute_cpu_reference_observed`); the public entry point is a thin wrapper around it, and the M6 and M5 tests exercise the public path.
- **Valgrind.** Clean on the runtime tests (chain, execution, output move, buffer storage, owned array). On `fusion_test` and `inplace_reuse_test` Valgrind reports no memory errors, but its own `operator new` replacement bypasses the tests' allocation counter, so their allocation checks cannot run under Valgrind.
- **Pre-existing, unrelated:** a Release (`-O3`) ASan+UBSan build with `-Werror` fails on a GCC `-Wmaybe-uninitialized` warning in `benchmarks/executor_breakdown.cpp`; canonical `main` fails identically. Without `-Werror` that configuration passes 23/23 on M7.

## 29. Findings

**MEASURED**
- Final-only chain, M7/M6: 0.811 (1M) and 0.817 (4M) under default glibc, 0.815 and 0.785 under T1 (GCC, 20/20 pairs each); Clang 0.78–0.82 (10/10).
- M7 final-only equals the native fused loop: 1.000 / 1.007 (GCC T1), 1.004 / 0.995 (GCC default), 0.98–1.03 (Clang). M6 was 1.23–1.25× native fused.
- Absolute saving 141–183 µs at 1M and 574–710 µs at 4M, the same under default, T1 and T2 (T2 noisier).
- Allocation count, fault counts (default, T1, T2), syscall counts and `brk` sequences: identical between M6 and M7 for every shape.
- Controls (single add, intermediate output, output after use): within the identical-code noise floor; not fused (observation seam).
- GCC and Clang vectorize the integrated fused loop for f32 and i32 (3 loads, 2 adds, 1 store per vector; no calls).
- Non-NaN results bit-identical to M6 and to the oracle; NaN payloads differ for some elements (GCC: 980 left / 2,764 right of 21,683 NaN results; Clang: 0 / 1,911).
- Small N: no detectable detector cost; fusible programs 9–32 ns faster at N = 256.
- 3- and 4-add chains: M7/M6 0.85–0.90, still 1.24–1.39× a native single loop.
- All mutations killed; C2 (correct forward search) only through the observation seam.

**INFERRED**
- With allocation and lifetime held constant, removing the materialized intermediate is what closes the remaining gap to the native fused loop: the time saving appears in every allocator configuration while faults and syscalls are unchanged.
- The 24 → 16 B/element model overstated the gain (0.667 predicted, 0.81 measured) because M6's in-place second pass already cost less than a full 12 B/element pass.

**FALSIFIED**
- "M7/M6 ≈ 16/24 = 0.667" as a prediction of time: measured 0.78–0.82. (The pre-registered plausible band 0.67–0.85 contains the result.)
- "The detector adds measurable small-N overhead": not observed.

**NOT YET KNOWN**
- Behavior on MSVC, Apple Clang, AArch64, other allocators, DRAM-resident sizes, other CPUs.
- Actual memory traffic (no PMU).
- Whether `FLT_EVAL_METHOD` is defined on MSVC (determines f32 fusion there).
- The benefit of fusing whole chains (not implemented).

## 30. Research conclusion

After M6 had already removed the intermediate's extra allocation, eliminating the physical intermediate bought **the entire remaining gap to the native fused loop** for the two-add chain: about 19 % of the M6 time (≈ 150 µs per 4 MiB result at 1M, ≈ 0.6–0.7 ms at 4M), on both compilers and under every allocator configuration, with the allocation count, fault counts, syscall pattern, logical peak storage and IR unchanged. The causal signature asked for was observed: one allocation, one pass instead of two, 16 instead of 24 B/element of payload, the same faults, and a time equal to the native fused bound. The naive 0.667 bandwidth ratio did not materialize; M6 was already closer to the bound than its payload model implied.

## 31. Merge recommendation

**MERGE CANDIDATE**, conditional on green CI on the PR (Ubuntu, macOS, Windows/MSVC, Ubuntu GCC ASan+UBSan), which has not run because no PR exists.

| Criterion | Status |
| --- | --- |
| strict detector correct | tests §11–12, mutations §13 |
| allocation count = M6 on primary workload | 1 = 1 (§15) |
| T2 fault sets = M6 | 1,025 = 1,025 at 1M, 4,097 = 4,097 at 4M (§18) |
| no materialized T on the fused path | §22 (five independent kinds of evidence) |
| f32 grouping preserved; i32 wrapping preserved | sentinels, rounding-sensitive data, mutation A; i32 boundaries |
| later output / intermediate output / non-adjacent / `T + T` block fusion | §12 |
| first-op and second-other M6-reusable operands block fusion | §12 |
| caller inputs unchanged; repeated execution stable | every test case; §11 |
| GCC and Clang integrated loops vectorize | §23–24 |
| sanitizers, all existing tests, Experiment 002 | 23/23 in each gate: GCC Release `-Werror`, Clang Release `-Werror`, GCC Debug ASan+UBSan `-Werror`, GCC `-O3` ASan+UBSan (without `-Werror`, §28) |
| small-N regression | none detected (§26) |
| primary large-N performance non-regressive | 0.79–0.82 (§16–17) |
| one-variable rule | allocation, faults, syscalls, peak, IR unchanged |

## 32. Candidate M8

**Fusion of single-use add chains longer than two.** The supplementary chain measurement (§27) is the only place where the measured workloads still leave a clear, attributable gap: with pairwise fusion, 3- and 4-add chains remain 1.24–1.39× a native single loop, because every add after the first pair is a separate in-place pass. M8 would fuse a maximal run of adjacent single-use adds (each intermediate used only by the next add, no output) into one loop with one result allocation, keeping M7's conservative exclusions, and measure it against M7 with the same allocation/fault invariants. Not implemented.
