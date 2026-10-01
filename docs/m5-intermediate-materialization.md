# Lume M5: Intermediate Materialization Baseline

M5 is observational. It changes nothing in `src/` or `include/`: the canonical M4 executor is measured as-is. The question is what it costs when a Lume value must exist in memory *between* two dependent operations. Lume's two-add chain is compared with native fused and two-pass loops and with an interpreter-free OwnedArray replica, so that arithmetic, interpretation and materialization can be separated.

Every number is from one machine (glibc 2.39, GCC 13.3.0, Clang 18.1.3, libstdc++ 13, baseline x86-64 without `-march`). None of it is a general performance claim.

## 1. Research question

For `D = A + B; E = D + C`, how much does materializing D cost, in time, modelled traffic, allocations and page faults, compared with a native fused loop `e[i] = (a[i] + b[i]) + c[i]`?

## 2. M4 baseline

Canonical `main` is `4c958a6149cfc9addc9a1ccfe749f4fa423187ee` (M0–M4). For a single `C = A + B`, M4 runs at about native speed: at N = 1M, native ≈ Lume ≈ 0.47 ms, with a model of 12 B/element. Re-measured here as `pxir_single_add_control`, M4 gives 471 µs at N = 1M (GCC default), and the M1–M4 checksum is reproduced exactly (D at N = 1M: `0xf09ed3431c02ceea`).

## 3. Workloads (f32, seed 42, A then B then C from one `mt19937_64` stream)

| Name | Lume program | Output semantics under M2 |
| --- | --- | --- |
| Final-only chain (primary) | `D = A + B; E = D + C; output E` | E moved (its final use) |
| Intermediate-output chain | `D = A + B; output D; E = D + C; output E` | D **copied** (it is read later), then E moved |
| Output-after-use (semantic control) | `D = A + B; E = D + C; output D; output E` | D moved (last use), E moved |
| Single-add control | `D = A + B; output D` | D moved |

N ∈ {1, 256, 4,096, 65,536, 1,048,576, 4,194,304}.

## 4. Hypotheses (stated before measuring)

| Id | Hypothesis | Outcome |
| --- | --- | --- |
| H1 | Lume final-only chain ÷ native fused ≈ 24/16 = 1.50 | **Supported in steady state (T1):** 1.54 at 1M and 1.52 at 4M. **Not under default glibc:** 4.32 at 1M and 5.34 at 4M, dominated by page faults (§14, §17). |
| H2 | Lume chain ≈ owned two-pass replica, so interpretation is small | **Supported.** T1 1M: 952 µs vs 946 µs. Default 1M: 2,687 vs 2,784 µs, with the same 2,016 faults per call. |
| H3 | Native two-pass ÷ native fused ≈ 1.5 at large N | **Supported:** 1.51–1.55 at 1M and 1.65–1.71 at 4M. It does not hold cache-resident (1.14 at N = 4,096). |
| H4 | The intermediate-output chain costs extra, corresponding to D's deep copy | **Supported.** T1 1M: +298 µs over the final-only chain (≈ 8 B/element; M1's isolated output copy was about 300 µs). With D output *after* use (moved), the cost is +1 µs. |
| Faults | Under forced mmap: about 1, 2 and 3 result-sized fault sets | **Supported.** T2 1M: owned fused 1,025, chain 2,050, chain + D 3,075 per call. |

## 5. Semantic lower bounds

- **Final output only:** `e[i] = (a[i] + b[i]) + c[i]`, a single pass: read A, B and C, write E.
- **Both outputs:** `d = a[i] + b[i]; d_out[i] = d; e[i] = d + c[i]`: read A, B and C, write D and E. The evidence shows `d` stays in a register (§20).
- **Evaluation order** is `(A + B) + C` everywhere: explicit parentheses, no `-ffast-math`. A dedicated test checks that Lume gives `(1 + 2⁻²⁴) + 2⁻²⁴ = 1`, while `1 + (2⁻²⁴ + 2⁻²⁴)` would be `1 + 2⁻²³`.

## 6. Data-movement model (minimum user-level payload, B/element)

| Path | Reads | Writes | Total |
| --- | --- | --- | ---: |
| Lume single add / native add | A, B | C | 12 |
| Native fused, owned fused | A, B, C | E | 16 |
| Native dual output | A, B, C | D, E | 20 |
| Native two-pass, owned two-pass, **Lume final-only chain** | A, B (pass 1); D, C (pass 2) | D, E | **24** |
| **Lume intermediate-output chain** | as above, plus D (copy) | D, D-copy, E | **32** |

- **Materializing D:** in the model this costs 8 B/element (write D, read D).
- **Copying D for output:** another 8 B/element (read D, write the copy).
- **Output moves:** 0 N-scaled payload bytes.
- **Not modelled:** RFO, cache-line granularity, the kernel's demand-zero page writes and allocator metadata.

## 7. Benchmark design

The benchmark is `benchmarks/intermediate_materialization.cpp` (`lume_bench_intermediate_materialization`).
- **Harness:** the M1 harness, **copied** so that `executor_breakdown.cpp` stays unchanged: 5 warmup and 51 measured samples; the median is reported; K = 1000 repetitions per interval for N < 4096 and K = 1 otherwise; results are kept until the interval ends and checked against the oracle outside the timed region; minor faults are read with `getrusage`.
- **Order:** forward and reverse component order, interleaved.

| Component | What is timed | Model |
| --- | --- | ---: |
| `native_fused_preallocated` | `e[i] = (a[i] + b[i]) + c[i]` into a preallocated E | 16 |
| `native_two_pass_preallocated` | `d = a + b` loop, then `e = d + c` loop, into preallocated D and E | 24 |
| `native_fused_dual_output` | `d = a + b; d_out = d; e = d + c` per element, into preallocated D and E | 20 |
| `owned_fused_allocate_plus_compute` | `OwnedArray::for_overwrite` E, then the fused loop | 16 |
| `owned_two_pass_materialized` | allocate D, D = A + B, allocate E, E = D + C (OwnedArray, the executor's loop shape); D and E stay alive until the interval ends | 24 |
| `input_validation_chain` | the executor's real input validation for the 3-input chain (K = 1000) | — |
| `pxir_single_add_control` | M4 executor, `D = A + B; output D` | 12 |
| `pxir_chain_final_only` | M4 executor, final-only chain | 24 |
| `pxir_chain_intermediate_output` | M4 executor, intermediate-output chain (checks 2 outputs: D, E) | 32 |
| `pxir_chain_output_after_use` | M4 executor, output-after-use control (checks D, E) | 24 |

**Replica fidelity:** the executor frees its internal D when `execute_cpu_reference` returns, which is inside the timed call. The replica keeps D alive until the interval ends and frees it untimed. In the default allocator both produced the same 2,016 faults per call at N = 1M. M1 measured the free itself at about 0.2 µs.

## 8. Correctness

- **Oracle:** the independent oracle computes D = A + B and E = D + C with its own loop; E is never computed through the executor.
- **Benchmark checks:** every timed result is checked exactly (bitwise for non-NaN values, any NaN matching any NaN) outside the timed region, including both outputs of the two-output programs. There is no `MISMATCH` in any of the 27 raw files.
- **Checksums:** identical in GCC and Clang at every N (for example N = 1M: D `0xf09ed3431c02ceea`, E `0x166ecc1712346145`).
- **New test** `lume_chain_test`:
  - all three chain shapes at N ∈ {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099}, with inputs checked unchanged;
  - the evaluation-order case;
  - f32 special values, including −0 + −0 + −0 = −0, and NaN/∞;
  - the i32 chain with wrap boundaries (hand-computed values that also agree with the oracle), plus random i32 at five lengths.
- **Smoke test:** the benchmark's CTest smoke test requires `correctness=exact` and fails on `MISMATCH`. It sets no performance threshold.
- **Mutation 1** (temporary, not committed): `E = D + C` → `E = D + B` in the benchmark's program. It was killed: the smoke test failed with `pxir_chain_final_only correctness=MISMATCH`. The first attempt was discarded: under `-Werror` the mutated source did not compile (unused `c`), so the test had run a stale binary. The mutation was rebuilt without `-Werror` and rerun.
- **Mutation 2** (temporary, not committed): `output D` removed from the intermediate-output program while the checker still expects [D, E]. It was killed: `pxir_chain_intermediate_output correctness=MISMATCH`.

## 9. Native fused baseline

Both GCC 13.3 (16-byte vectors) and Clang 18.1 (width 4, interleave 2) vectorize it. The GCC loop body per 4 floats is 3 `movups` loads, 2 `addps` and 1 store, so the intermediate is never written (see [`codegen_evidence.txt`](data/m5/codegen_evidence.txt)). The owned fused loop compiles to the same shape and costs the same as the preallocated loop at large N (0.99× at 1M, default).

## 10. Native two-pass baseline

Both loops are vectorized by both compilers (each: 2 loads, 1 `addps`, 1 store), so the two-pass penalty is not a vectorization artifact. Two-pass ÷ fused:

| N | GCC default | GCC reverse | GCC T1 | Clang |
|---:|---:|---:|---:|---:|
| 4,096 | 1.14 | 1.15 | — | 1.24 |
| 65,536 | 1.51 | 1.47 | 1.59 | 1.82 |
| 1,048,576 | 1.51 | 1.55 | 1.53 | 1.51 |
| 4,194,304 | 1.70 | 1.71 | 1.65 | 1.69 |

## 11. Lume final-only chain

| N | Default: chain / fused | Default: chain / two-pass | Faults/call | T1: chain / fused | T1: chain / two-pass |
|---:|---:|---:|---:|---:|---:|
| 4,096 | 1.60 | 1.40 | 0 | — | — |
| 65,536 | 1.36 | 0.90 | 0 | 1.41 | 0.88 |
| 1,048,576 | **4.32** | 2.85 | **2,016** | **1.54** | **1.01** |
| 4,194,304 | **5.34** | 3.15 | **8,160** | **1.52** | 0.92 |

Execution evidence that each Lume add runs separately and materializes D:
- the executor code is unchanged M4 (`add_buffers`, one `OwnedArray` per `add`, vectorized per M4 §20–21);
- under T2 the chain takes exactly twice the single-output faults (2,050 vs 1,025 at 1M), i.e. two first-written result allocations;
- strace shows two 4,198,400-byte allocations per call (§18);
- the output is exact, and E depends on D (mutation 1).

## 12. Lume intermediate-output chain

| N = 1M | Default | T1 |
| --- | ---: | ---: |
| Final-only chain | 2,687 µs | 952 µs |
| **Chain + output D before use (copy)** | **4,302 µs** (3,040 faults) | **1,251 µs** |
| Chain + output D after use (move) | 2,731 µs (2,016 faults) | 953 µs |
| Native dual output (20 B model) | 816 µs | 818 µs |

- **Cost of copying D:** in T1, the D copy adds 298 µs (≈ 8 B/element at about 28 GB/s). When D's output comes after its use, M2's move applies and the extra cost is ≈ 1 µs.
- **Against the dual-output bound:** Lume chain + D is 2.03× fused in T1 (model 32/16 = 2.0), and 1.53× native dual output (model 32/20 = 1.6).

## 13. Fixed executor costs

- **Validation:** `input_validation_chain` (3 inputs) takes 132–144 ns, against about 88 ns for 2 inputs (M1). The difference is associated with the third input's validation, including its diagnostic string.
- **Small N:** the chain adds 76 ns over the single add at N = 1 (§22). That compares programs with **different input counts**, so it includes the extra input's validation (about 53 ns) as well as the extra operation.
- **Large N:** the fixed costs (≤ 0.3 µs) are negligible at N ≥ 65,536.

## 14. Default allocator results (GCC, 6 forward runs; medians)

| N | Native fused | Two-pass | Dual | Owned fused | Owned 2-pass | Lume single | **Lume chain** | Lume chain + D | After-use |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 ‡ | 1.3 ns | 4.1 ns | 3.2 ns | 10.6 ns | 21.4 ns | 175 ns | 257 ns | 306 ns | 280 ns |
| 256 ‡ | 43 ns | 69 ns | 131 ns | 255 ns † | 615 ns † | 270 ns | 348 ns | 464 ns | 418 ns |
| 4,096 | 832 ns | 949 ns | 1,944 ns | 862 ns | 1,193 ns | 835 ns | 1,332 ns | 1,708 ns | 1,330 ns |
| 65,536 | 14.4 µs | 21.8 µs | 32.3 µs | 13.7 µs | 22.6 µs | 8.4 µs | 19.6 µs | 35.7 µs | 22.9 µs |
| 1,048,576 | 622 µs | 942 µs | 816 µs | 616 µs | 2,784 µs | 471 µs | **2,687 µs** | 4,302 µs | 2,731 µs |
| 4,194,304 | 2,539 µs | 4,307 µs | 3,635 µs | 2,494 µs | 13,601 µs | 1,856 µs | **13,565 µs** | 22,913 µs | 13,731 µs |

- ‡ Measured on the batched K = 1000 path (M1 §4 caveat). † A heap-growth order artifact (0.2–0.45 faults per op in forward order; the owned two-pass takes 128 ns in the reverse runs).
- **Reverse runs** (3) agree: chain/fused 1.44 (65,536), 4.56 (1M), 5.38 (4M).
- **Clang** (3 runs, default): chain/fused 1.71 (4,096), 9.18 (65,536, **96 faults** per call, which GCC did not have at this N), 4.51 (1M), 5.34 (4M); two-pass/fused 1.51 at 1M.

## 15. T1 (`trim_threshold=1 GiB`, `mmap_threshold=32 MiB`; 3 runs)

| N | Native fused | Two-pass | Owned 2-pass | Lume chain | Chain/fused | Lume chain + D | Chain+D/fused |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 65,536 | 14.0 µs | 22.3 µs | 27.1 µs | 19.7 µs | 1.41 | 37.7 µs | 2.69 |
| 1,048,576 | 617.5 µs | 942.4 µs | 945.7 µs | **952.3 µs** | **1.54** | 1,250.6 µs | 2.03 |
| 4,194,304 | 2,501.6 µs | 4,139.3 µs | 4,333.5 µs | **3,810.7 µs** | **1.52** | 5,879.8 µs | 2.35 |

No build faults under T1. At N ≥ 1M, the chain's cost relative to the fused loop matches the 24/16 byte model within 3%, and the chain's absolute cost equals native two-pass (1.01× at 1M) and the owned replica.

## 16. T2 (`mmap_threshold=128 KiB`; 2 runs)

| N | Owned fused | Owned 2-pass | Lume chain | Lume chain + D | After-use |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 77.4 µs (65) | 148.5 µs (130) | 176.2 µs (130) | 256.9 µs (195) | 148.3 µs (130) |
| 1,048,576 | 1,567 µs (1,025) | 2,803 µs (2,050) | 2,881 µs (2,050) | 4,469 µs (3,075) | 2,793 µs (2,050) |
| 4,194,304 | 6,874 µs (4,097) | 14,515 µs (8,194) | 13,796 µs (8,194) | 24,588 µs (12,291) | 13,956 µs (8,194) |

The number in parentheses is minor faults per call. Each first-written result allocation costs one set of about N/1024 + 1 faults: 1 set for fused, 2 for the chain, 3 for chain + D.

## 17. Page faults

| N | Configuration | Owned fused | Lume chain | Lume chain + D |
|---:|---|---:|---:|---:|
| 1M | Default (benchmark) | 0 | **2,016** | **3,040** |
| 1M | T1 | 0 | 0 | 0 |
| 1M | T2 | 1,025 | 2,050 | 3,075 |
| 4M | Default (benchmark) | 0 | 8,160 | 12,256 |

- **Pattern:** the default-allocator counts follow `k × pages − 32`, with k = 2 (D and E) or 3 (D, D-copy and E). That is the same glibc trim/re-fault pattern M1 found for a result plus its output copy.
- **History dependence (measured):** the standalone probe (§18) shows the **intermediate-output** chain **not** re-faulting after the first call under default glibc (307 faults per call on average over 10 calls, all on the first). The benchmark shows 3,040 on every call. The final-only chain re-faults on every call in both. So whether glibc trims depends on the allocator's history, in addition to the program's allocation pattern.

## 18. strace

Probe: [`strace_probe_chain.cpp`](data/m5/strace_probe_chain.cpp), 10 calls at N = 1M, each result destroyed at the end of its iteration. The traces and the probe's own fault counts are in `docs/data/m5/strace_*`.

| Program | Default glibc | T2 |
| --- | --- | --- |
| Final-only chain | 30 `brk`: per call +4 MiB (D), +4 MiB (E), −8 MiB (trim); 2,019.5 faults per call | 20 × `mmap`/`munmap` of 4,198,400 B (2 per call); 2,050.2 faults per call |
| Intermediate-output chain | 3 `brk` in total (heap growth on the first call, then reuse); 307.4 faults per call on average | 30 × `mmap`/`munmap` (3 per call); 3,075.2 faults per call |

Two result-sized allocation lifetimes per call are externally visible for the final-only chain, and three for the intermediate-output chain.

## 19. Logical peak payload (executor-owned, excluding borrowed inputs; a model, not measured RSS)

| Program | Live together at peak | Logical peak |
| --- | --- | ---: |
| Single add | C | 4 B/element |
| Final-only chain | D and E (E is written while D is read) | 8 B/element |
| Intermediate-output chain | D, D-copy (already in `outputs`), E | 12 B/element |

**Lifetime observation (from the code, not optimized in M5):** the executor frees an owned intermediate only when `execute_cpu_reference` returns, not at its last computational read. D therefore stays allocated after the second add has read it. In longer chains, every intermediate would stay live until the call returns, so the logical peak would grow with chain length. This is lifetime and reclamation, which is distinct from materialization, although in this workload both come from D's existence as a separate buffer.

## 20. GCC code generation (13.3, `-O3`, no `-march`)

Evidence is in [`codegen_evidence.txt`](data/m5/codegen_evidence.txt): the reports and every vectorized loop body.
- **Vectorized with 16-byte vectors (plus 8-byte epilogues):** native fused (line 194), both two-pass loops (199, 200), dual output (205), owned add (215) and owned fused (220).
- **Fused:** 3 loads, 2 `addps`, 1 store.
- **Two-pass:** 2 loops of 2 loads, 1 `addps`, 1 store.
- **Dual output:** 3 loads, 2 `addps`, 2 stores. The first `addps` result is stored to `d_out` and then used directly by the second `addps` (`movups %xmm0,(d_out)`, `addps %xmm3,%xmm0`), so **d is not reloaded**.

## 21. Clang code generation (18.1.3)

All six loops are vectorized, width 4: interleave 2, except dual output at interleave 1.
- **Fused:** 6 loads, 4 `addps` and 2 stores per iteration (two vectors).
- **Two-pass:** 2 loops.
- **Dual output:** `addps %xmm0,%xmm1` → store `%xmm1` to `d_out` → `addps %xmm1,%xmm0`, with no reload of d.

### Dual-output anomaly (measured, cause not identified)

With both compilers, the native dual-output loop is **2.2–3.0× native fused at cache-resident sizes** (N = 4,096 and 65,536). That is slower than two-pass (1.1–1.8×), although it moves fewer model bytes. At N ≥ 1M it behaves as modelled (1.28–1.48×).

Supplementary probes ([`dual_output_probe.cpp`](data/m5/dual_output_probe.cpp), [`dual_output_offset_probe.cpp`](data/m5/dual_output_offset_probe.cpp); results in `dual_output_probe_results.txt`) rule out:
- the runtime aliasing check (`__restrict` does not change it);
- the ordering of stores and loads (storing E first does not change it);
- the relative address offsets between `d_out`, `c` and `e`, which account for ≤ 25% at most.

The remaining cause (for example two interleaved store streams in L1/L2 on this virtualized CPU) could not be identified without hardware performance counters. The effect does not touch the Lume measurements, but the native dual-output bound is only meaningful at N ≥ 1M.

## 22. Small N (10 runs × 201 iterations; paired within each run; batched K = 1000)

| N | Single add | Chain | Chain − single (median, range; chain slower in) | Chain + D − chain | After-use − chain |
|---:|---:|---:|---:|---:|---:|
| 1 | 171.5 ns | 248.3 ns | **+76.2 ns** (+62.9 to +208.6; 10/10) | +40.0 ns | +23.0 ns |
| 256 | 271.4 ns | 366.8 ns | **+95.9 ns** (+63.2 to +105.0; 10/10) | +93.4 ns | +51.9 ns |

At N = 1, one extra operation plus one extra input adds about 76 ns, of which about 53 ns is associated with the additional input's validation (§13). Small-N results are not interpreted through the byte model.

## 23. Threats to validity

- **Everything from M1–M4 still applies:** one shared VM, no frequency control, a reported 260 MiB L3 (large-N rates are consistent with cache residency but not proven), no hardware counters, the timer floor (28–30 ns), and small-N batching.
- **Allocator dependence and history:** default-allocator results at N ≥ 1M are dominated by glibc's trim/re-fault behavior, which depends on allocation history (§17). This showed up as a difference between benchmark and probe, and between GCC and Clang at N = 65,536 (0 vs 96 faults). **T1 is the configuration that measures materialization itself.** Other allocators and operating systems were not measured.
- **Component layout and order:** all components run in one binary in a fixed order. Forward and reverse runs agree on every conclusion; some N = 256 allocations show a heap-growth order artifact (†).
- **Replica lifetime:** the owned two-pass replica frees D after the interval, while the executor frees it inside the call (§7).
- **Mid-N cache effects:** at N = 65,536 the chain (19.6 µs) runs *faster* than native two-pass (21.8 µs), and the single add (8.4 µs) runs faster than native fused (14.4 µs). The components use different buffers and cache states at L2-sized working sets, so mid-N ratios should not be read as exact shares.
- **Native dual-output anomaly:** unexplained at cache-resident N (§21).
- **Evaluation order:** fused baselines keep `(A + B) + C`. A reassociated native loop would not be a valid comparison.
- **Compilers:** GCC 13.3 and Clang 18.1 at `-O3` without `-march`; wider ISAs were not tested.

## 24. Findings

**MEASURED**

- **Steady state (T1):**
  - the Lume final-only chain is 1.54× native fused at 1M and 1.52× at 4M (model 1.50);
  - it equals native two-pass (1.01× at 1M) and the interpreter-free owned replica (952 vs 946 µs);
  - Lume chain + D is 2.03× fused (model 2.00).
- **Default glibc:**
  - the Lume chain is 4.32× fused at 1M and 5.34× at 4M, with 2,016 / 8,160 faults per call;
  - the owned replica shows the same faults and time, so the cost is not interpretation;
  - strace shows the +4 / +4 / −8 MiB `brk` pattern on every call.
- **T2:** 1, 2 and 3 first-written result fault sets per call for fused, chain and chain + D.
- **Copying D for output** costs ≈ 300 µs at 1M in T1; moving it after its use costs ≈ 1 µs.
- **Code generation:** all native and owned kernels are vectorized by both compilers; dual output keeps d in a register.
- **Small N:** the chain adds 76 ns at N = 1 (with one more input).
- **Correctness:** all correctness checks, both mutations and the checksums pass.

**INFERRED**

- **Steady state:** in steady state, intermediate materialization explains essentially all of the chain's gap to a fused loop on this machine. The measured extra ≈ 325 µs at 1M matches 8 B/element at the observed about 27 GB/s.
- **Default glibc:** the larger cost is the allocator trim/re-fault triggered by having two result-sized buffers freed together. About 1.7 ms of the 2.7 ms at 1M is associated with it. This is the M1 mechanism, re-triggered because materialization adds a second concurrent allocation, which M2's move had removed for single-op programs.
- **Interpretation** (dispatch, validation, bookkeeping) is not a meaningful part of the large-N cost.

**FALSIFIED**

- "The two-pass penalty is a code-generation artifact": both passes are vectorized.
- "Copying D for output is unavoidable whenever D is an output": when D's output follows its last use, M2's move makes it essentially free.

**NOT YET KNOWN**

- The cause of the native dual-output slowdown at cache-resident sizes.
- Behavior with other allocators and operating systems, and the exact glibc history conditions that decide trimming.
- Actual DRAM and cache traffic.
- Behavior of longer chains and of peak memory (RSS was not measured).

## 25. Research conclusion

**How much does intermediate materialization cost?** On this machine, for `D = A + B; E = D + C` at N ≥ 1M:
- **In steady state**, materializing D costs about **0.5× a fused loop's time** (1.52–1.54× fused against the 1.50 byte model). The Lume executor adds nothing measurable beyond a native two-pass loop that materializes D the same way.
- **Under the default glibc allocator**, the dominant cost is **not** the 8 B/element of D's traffic. It is the allocator trim/re-fault cycle that D's separate allocation triggers when D and E are freed together: 2,016 extra faults per call, about 65% of the chain's time at 1M. Removing D's traffic alone (fusion) would also remove D's allocation. But the fault cost is a lifetime and allocator interaction that could be tested separately from the bytes.

This matches interpretation-matrix **outcome A in steady state**, with a large allocator-lifetime component under default glibc that must be separated before a fusion experiment can be attributed cleanly.

**Research record recommendation: RESEARCH RECORD CANDIDATE.** Runtime unchanged; all correctness and sanitizer gates pass; raw evidence committed.

## 26. Candidate M6

Recommended M6 experiment: **last-use in-place result reuse.**
- **What:** when an `add` reads an executor-owned operand for the last time (e.g. D in `E = D + C`), write the result into that operand's storage instead of allocating a new buffer.
- **Measure:** with this harness, under default, T1 and T2.
- **Why this one, and not fusion first:** at N ≥ 1M under default glibc, the dominant measured cost is the allocator trim/re-fault from a second concurrent result allocation, not D's 8 B/element.
  - In-place reuse changes exactly that variable: one result allocation per chain instead of two, and a logical peak of 4 instead of 8 B/element.
  - It leaves the modelled traffic unchanged (24 B/element).
  - So it separates the allocator and lifetime effect from the byte effect. The prediction is that faults vanish under default glibc, time approaches the T1 level (≈ 0.95 ms at 1M), and T1 is unchanged.
  - Only after that separation would a fusion experiment (24 → 16 B/element) measure what fusion alone buys.
- **Status:** not implemented.
