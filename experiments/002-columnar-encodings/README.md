# Experiment 002: columnar encodings for an event-log workload

> Status: **measured. T1 passed, T2 failed: the pre-registered hypothesis is not supported (a trade-off).** Sections 1-7 (hypothesis, thresholds, method) were written before any measurement and were not edited afterwards; only this status line and sections 8-11 were filled in after the measurements. Analysis that goes beyond the pre-registered questions is labelled *post hoc*.

This is the first experiment aimed at H1 (representation density) and H2 (data movement) of [`docs/research.md`](../../docs/research.md) on a workload that is not a synthetic vector add.

## 1. Question

Can an append-only event log be held in memory with materially fewer bytes per row than a plain typed columnar layout, without losing any information, and without making scans or point lookups unacceptably slower?

## 2. Hypothesis and pre-registered thresholds

Candidate **E1** (dictionary + bit packing + delta, all lossless) is compared with baseline **B1** (typed columnar layout, no encoding).

- **T1 (footprint):** `bytes_per_row(E1) <= 0.50 * bytes_per_row(B1)`.
- **T2 (scan cost):** `max(S1, S2, S3)` of `median_time(E1) / median_time(B1)` `<= 1.25`, where S1-S3 are the three scan queries in section 6. The worst query decides, not the average.
- **Correctness gate (not a threshold):** every layout reproduces every logical row bit for bit, on the generated workload and on adversarial inputs. A failure here invalidates all measurements.

The hypothesis is **supported** only if T1 and T2 both hold. If T1 fails, the encodings do not pay on this workload. If T1 holds and T2 fails, the result is a trade-off and is published as one (the research charter lists "smaller encodings whose decode cost dominates" as a falsifier). No threshold changes after measurement. Any analysis added after seeing results is labelled post hoc.

**Informational, no threshold:** comparison with the stronger baseline B2, point-lookup cost (P1), build cost, and the order-0 entropy bound.

## 3. Workload (deterministic generator)

The workload is synthetic and reproducible from a seed. It is calibrated to aggregate statistics only (cardinality, marginal entropy, session length, timing gaps, null and constant columns) of a small real event log (about 3 thousand rows, about 200 sessions). No raw data, identifiers, column values or product names from that log are included in this repository.

| Logical column | Type | Model |
| --- | --- | --- |
| `event_id` | 128-bit | uniform random |
| `session_id` | 128-bit | one per session; rows are clustered by session |
| `client_seq` | int64 | 0,1,2,... within a session; a skip (gap of 1..14) with probability 0.3% per event |
| `schema_version` | uint32 | constant |
| `event_name` | text (10 B) | 14 values, Zipf calibrated to 2.157 bits of entropy |
| `route` | text (10 B) | 51 values, Zipf calibrated to 3.403 bits |
| `properties` | text (10-31 B, mean about 20.5) | 102 values, Zipf calibrated to 4.022 bits |
| `acquisition` | text (2 or 7 B) | 2 values, 0.004 bits |
| `mode` | text (4 B) | constant |
| `occurred_at` | int64 ms | session start uniform over 30 days; gap between events log-normal (median 0.44 s, p90 about 30 s, capped at 58,600 s) |
| `received_at` | int64 ms | true time plus latency, log-normal (median 106 ms, p90 about 1.2 s, capped at 2,751 ms) |

Client clock skew: 0.5% of sessions have `occurred_at` shifted forward by 60-190 s, which makes `received_at - occurred_at` negative for those sessions. Session length is log-normal (median about 5.5, p90 about 37, clamped to 1-200).

**Known simplification:** columns are drawn independently. Real columns are correlated (for example route and event name), so cross-column effects are not modelled.

## 4. Layouts

All layouts hold the same logical rows, clustered by session (rows of a session are contiguous, in `client_seq` order). All are lossless for arbitrary input, including adversarial values.

- **B1, typed columnar (baseline for the thresholds):** 128-bit ids as two `uint64`; integers narrowed to the smallest of int8/16/32/64 that fits the column range; timestamps as raw int64; text as Arrow-style `uint32` offsets plus one byte buffer. No dictionary, no delta, no bit packing.
- **B2, dictionary columnar (strong baseline, informational):** B1, plus a dictionary for every text column and for `session_id`, with byte-aligned indices (uint8/16/32). Timestamps stay raw.
- **E1, encoded (candidate):**
  - `session_id`: run-length: one id and one `uint32` start offset per run of equal ids;
  - `client_seq`: `zigzag(seq - expected)` where `expected` is 0 for the first row of a run and `previous + 1` otherwise, in 128-row blocks with a per-block bit width;
  - `occurred_at`: one absolute anchor per run, then `zigzag(delta from previous row in the run)`, block-packed;
  - `received_at`: stored as `zigzag(received_at - occurred_at)`, block-packed;
  - text columns, `schema_version`: dictionary plus a fixed bit width of `ceil(log2(cardinality))` (0 bits for a constant column);
  - `event_id`: raw 128 bits (random ids are incompressible). Bytes are also reported without it, because whether it is redundant is a schema decision, not an encoding one.

Every byte a layout needs to be read back is counted: dictionaries, offsets, per-block widths and offsets, run tables and padding.

## 5. Correctness policy

`roundtrip_test` checks, for B1, B2 and E1: every row field by field against the source, S1-S3 results, and point lookups, on generated workloads of several sizes (including block-boundary sizes 127/128/129) and seeds, and on adversarial workloads: extreme int64 values (full 64-bit deltas), negative and huge sequence numbers, sessions that are not clustered, one session for all rows, a distinct session per row, empty strings, strings with embedded NUL bytes, and empty and single-row tables. The benchmark refuses to time anything until all layouts agree on every query result.

## 6. Queries

- **S1:** `count(*) where event_name = T` (one column).
- **S2:** `sum(received_at - occurred_at) where event_name = T`, in wrapping uint64 arithmetic (two columns plus the filter).
- **S3:** `max(occurred_at)` (worst case for E1: it must rebuild every timestamp from deltas).
- **P1:** random point lookup of a full row (200,000 random rows). Reported, no threshold.

`T` is the most frequent `event_name`.

## 7. Measurement procedure

- Release build, run with GCC and Clang.
- Footprint is exact byte accounting of the layout buffers (section 4), not sampled RSS.
- Timing: rounds of interleaved A/B/C runs with the layout order rotated each round; the median over rounds is reported, with min and max. Every query result is consumed so it cannot be eliminated.
- Build (encode) time is measured once per layout, informational.
- Environment (OS, CPU, RAM, compiler, flags, commit SHA, N, seed) is printed by the benchmark.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pxir_exp_002 pxir_exp_002_roundtrip
ctest --test-dir build -R exp002
./build/experiments/002-columnar-encodings/pxir_exp_002 rows=1000000 seed=42 rounds=15
```

## 8. Results

Raw outputs: [`results/`](results/). Environment: Intel Xeon @ 2.10 GHz (2 vCPUs, virtualized), 8 GB RAM, Linux, GCC 13.3.0 and Clang 18.1.3, `-DCMAKE_BUILD_TYPE=Release`, base commit `bd338ba` plus this experiment. Main configuration: `rows=1000000 seed=42 rounds=15`. All runs passed the correctness gate (`correctness=exact`); `roundtrip_test` passes with GCC and Clang, Release and ASan+UBSan.

### 8.1 Footprint (exact bytes; identical on every compiler and run)

| Layout | Bytes/row | Ratio to B1 | Without `event_id` |
| --- | ---: | ---: | ---: |
| B1 typed columnar | 115.17 | 1.000 | 99.17 |
| B2 dictionary columnar | 43.02 | 0.373 | 27.02 |
| **E1 encoded** | **24.77** | **0.215** | **8.77** |

E1 by column (bytes/row): `event_id` 16.000, `occurred_at` 3.353, `received_at` 1.694, `session_id` 1.267, `properties` 0.877, `route` 0.751, `event_name` 0.500, `client_seq` 0.205, `acquisition` 0.125, `schema_version` and `mode` 0.

**T1: PASS.** E1 / B1 = 0.215 (threshold 0.50). Across runs and scales the ratio stays in 0.207-0.215.

### 8.2 Query cost, median ns per row (P1: per lookup), GCC, 1,000,000 rows

| Query | B1 | B2 | E1 | E1 / B1 | E1 / B2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| S1 filter + count | 6.80 | 0.21 | 0.91 | 0.134 | 4.28 |
| S2 filter + sum | 6.81 | 3.22 | 5.04 | 0.739 | 1.56 |
| S3 max(occurred_at) | 0.56 | 0.55 | 2.25 | **4.000** | 4.06 |
| P1 point lookup | 268 | 202 | 412 | 1.540 | 2.04 |

Ratio E1 / B1 in every configuration measured:

| Run | S1 | S2 | S3 | P1 |
| --- | ---: | ---: | ---: | ---: |
| GCC, 1M, seed 42, run 1 | 0.134 | 0.739 | 4.000 | 1.540 |
| GCC, 1M, seed 42, run 2 | 0.122 | 0.690 | 4.097 | 1.569 |
| Clang, 1M, seed 42, run 1 | 0.123 | 0.734 | 3.872 | 1.662 |
| Clang, 1M, seed 42, run 2 | 0.122 | 0.730 | 3.771 | 1.739 |
| GCC, 200k, seed 7 | 0.129 | 0.739 | 4.190 | 1.748 |
| GCC, 3M, seed 11 | 0.130 | 0.654 | 3.292 | 1.442 |

**T2: FAIL.** The worst scan is S3 in every configuration: 3.3-4.2x B1 (threshold 1.25).

### 8.3 Informational

- **Build time**, 1M rows, GCC: B1 99 ms, B2 93 ms, E1 109 ms.
- **Against the strong baseline B2:** E1 needs 0.576x its bytes (1.74x smaller), but it is slower than B2 on all four queries: S1 2.3-5.2x, S2 1.5-1.6x, S3 3.3-4.2x, P1 2.0-2.3x. The S1 ratio varies with the compiler because B2's S1 takes 0.21 ns/row with GCC and 0.34 ns/row with Clang; the cause was not investigated (no code generation inspection).
- **Order-0 entropy vs bits E1 uses per row:**

| Column | Entropy | E1 |
| --- | ---: | ---: |
| `event_name` | 2.158 | 4.002 |
| `route` | 3.405 | 6.006 |
| `properties` | 4.021 | 7.019 |
| `acquisition` | 0.004 | 1.000 |
| `client_seq` (gap) | 0.040 | 1.643 |
| `received_at - occurred_at` | 9.162 | 13.549 |

## 9. Analysis

**Pre-registered outcome.** T1 holds by a wide margin and T2 fails, so the hypothesis is not supported as stated: on this workload the encodings pay in memory and cost scan and lookup time in the worst case. The charter lists exactly this ("compact layouts that make required random access slower") as a falsifier. The result is a trade-off, and it is reproducible across two compilers, two runs, three seeds and three sizes.

**Post hoc observations** (not pre-registered; treat as hypotheses to test, not as findings):

1. **Where the bytes go.** `event_id` is 64.6% of E1 (16 of 24.77 B/row). Timestamps are 20.4%, session runs 5.1%, the four dictionary columns 9.1%, `client_seq` 0.8%. A random 128-bit id is incompressible; whether it is redundant is a schema decision, not an encoding one (in this workload `(session_id, client_seq)` is already unique by construction, but a real system may need the id as an idempotency key). Without it, E1 is 8.77 B/row: 11.3x smaller than B1 and 3.1x smaller than B2.
2. **Entropy headroom is small.** Fixed-width dictionary codes use about 8.4 bits/row more than the order-0 entropy of the four dictionary columns, which is about 1.05 B/row (4.3% of E1). An entropy coder would not change the picture.
3. **The scan failure is one query.** S3 must rebuild every timestamp from deltas, while B1's S3 is a plain scan of an int64 column that is bandwidth-bound. E1's S3 costs about 2.2 ns/row (roughly 4.7 cycles at 2.1 GHz). The mechanism was not isolated; a serial dependency (running sum) and a per-row run-boundary check are candidates, and no profiler or hardware counters were used.
4. **E1 is faster than B1 where the encoding helps.** Comparing a 4-bit code is 7-8x faster than comparing strings (S1), and S2 reads one packed column instead of two int64 columns. Against B2, whose byte-aligned codes cost almost nothing to compare, bit unpacking is a real cost.
5. **The implementation is unoptimized.** E1 is scalar code with no SIMD, no unrolled unpackers and per-element indexing in S1. The verdict is about this implementation, not about a bound.

## 10. Limitations

- Synthetic data; columns are drawn independently, so cross-column correlation is not modelled, and real timestamps may not be log-normal.
- The generator was calibrated to a real log of only about 3 thousand rows, so its statistics have sampling error, and the tail behaviour (session length, clock skew rate) at scale is assumed, not observed.
- One machine (virtualized 2-vCPU Xeon), no hardware counters, no CPU pinning. The T2 failure is far outside any plausible noise (3.3-4.2x against 1.25x), but small ratios are noisy.
- Footprint is exact byte accounting of the layout buffers, not measured process RSS, and it excludes allocator overhead.
- No general-purpose compressor (Zstd, LZ4) baseline and no Arrow implementation: B2 is an Arrow-like proxy, not Arrow. Compressors are not directly queryable, so they answer a different question.
- Every layout assumes rows are clustered by session. Data that arrives interleaved needs a sort or compaction step whose cost is not measured here.
- Queries are three scans and one lookup; range predicates, joins, updates and deletes are not covered. The layouts are immutable (build once).
- Not verified: MSVC and macOS builds, concurrent readers, and behaviour on corrupted encoded buffers (the decoders assume well-formed input; only the encoders validate widths).
- Correctness was checked by round trip, and the checker was mutation-tested (six injected bugs, all detected), but that is evidence about these implementations, not a proof.

## 11. Conclusion

- **Supported:** on this event-log workload, dictionary + bit packing + delta reduces memory to 0.215x a plain typed columnar layout (T1), and to 0.576x a dictionary-encoded columnar layout, with no information loss.
- **Not supported:** the same encoding stays within 1.25x of B1 scan cost (T2). The worst case is 3.3-4.2x on a query that must rebuild every timestamp, and point lookups are 1.4-1.7x slower.
- **Open:** whether an implementation with independently decodable blocks and vectorized timestamp reconstruction closes the S3 gap. That is a new hypothesis and needs its own pre-registered thresholds (a variant E2), not a retroactive change to T2. Which queries the product actually runs (for example time-range filters answered from block metadata instead of reconstructing every timestamp) should decide whether S3 is the right worst case to optimize.
