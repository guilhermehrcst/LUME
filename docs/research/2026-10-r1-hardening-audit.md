# E2-R1 hardening and adversarial audit (October 2026)

Branch `lume/e2-r1-resource-preflight-v01`, base `main` = `b204b351`. Local only, not pushed. This is an audit record, not a preregistration; no preregistered gate, metric, threshold or historical datum was changed. Evidence classes: OBSERVED (read in code or output), MEASURED (executed here), INFERRED (reasoned, not executed), HYPOTHESIS.

## 1. Commits on the branch

| commit | change | verified by |
| --- | --- | --- |
| `2f34035` | RAM preflight: primary refused below 8 GiB `MemAvailable` (8,388,608 kB, the kernel's 1024-byte "kB"); smoke allowed and labelled non-primary | 32 checks, 12/12 mutations, namespace end-to-end |
| `95b5691` | swap evidence: `pswpin`/`pswpout` validated in both snapshots; missing, duplicate, malformed, negative, >18-digit or decreasing counters are invalid evidence, never zero; primary refused on positive (exit 9) or invalid (exit 10) | 88 checks, 19/19 mutations, verbatim gate execution, namespace end-to-end |
| `bbc6e28` | analysis: a machine is accepted only with both swap deltas exactly `"0"`; a missing or empty `swap_activity.txt` or a missing key now gives INCONCLUSIVE | self-test, the new checks fail on the previous analyzer |
| `a3a288e` | publication: re-check, `mv -T`, noclobber tarball and checksum; a result appearing during the run is refused (exit 11), never nested into or overwritten | verbatim publication tests incl. each barrier alone, 4/4 mutations, real race end-to-end |

Independent differential check of both preflight helpers against a separate Python reading of the specification: 6,000 random and adversarial inputs (two seeds), 0 disagreements, no shell arithmetic error ever reached stderr (MEASURED). Full suite: 35/35 on GCC 13.3 Release, Clang 18.1 Release and GCC Debug with ASan+UBSan and warnings as errors (MEASURED).

## 2. Findings

| ID | Sev. | Finding | Status |
| --- | --- | --- | --- |
| F1 | P1 | Swap counters missing, empty or `2^64` gave delta 0 and a primary run would have been published as zero swap | fixed `95b5691` |
| F2 | P1 | No RAM check: a host below the preregistered 8 GiB could start a primary run and publish a normal-looking bundle | fixed `2f34035` |
| F3 | P2 | Analyzer read a missing/empty swap record as zero swap and issued a scientific verdict | fixed `bbc6e28` |
| F4 | P2 | Publication could nest into / overwrite a same-id result that appeared during the run, exiting 0 | fixed `a3a288e` |
| F5 | P2 | E1 binaries' recorded SHA is configure-time, not build-time | documented, not changed |
| F6 | P3 | Executor: internal policy `{fuse, recompute}` fails on some verified programs | proposal with tested patch |
| F7 | P3 | Analyzer verifies neither `SHA256SUMS`, nor `resources.txt`, nor the smoke flag | proposal |
| F8 | P3 | E1 results README omits the `--with-supplementary` flag needed to regenerate `gcc_fresh_default/derived` | proposal |

**F1.** Evidence: `run_replication.sh` at `2f34035`, `vm()` + `$((IN1-IN0))`. Reproduction: the runner's verbatim lines on fixture snapshots; missing `pswpin`, both counters missing, empty value and `18446744073709551616` all yielded `PUBLISH-AS-ZERO-SWAP`; non-numeric, duplicate and `09` aborted with a bare shell error; `010` was read as octal. Impact: the preregistered "no active swap" could be reported as satisfied without evidence. Fix and verification: section 1.

**F3.** Evidence: `analyze_cross_machine.py`, `m["swap"].get("pswpout_delta", "0") not in ("0", "")`. Reproduction: the self-test's own synthetic bundles with `swap_activity.txt` deleted or empty gave `PORTABLE STATIC POLICY SUFFICES`. Impact: a bundle without swap evidence (manual import, older tooling, truncated transfer) would yield a verdict. Fix: require `"0"` for both deltas. This reads the preregistered INCONCLUSIVE condition "active swap" together with section 2's requirement "no active swap during the run": absence must be shown. It adds no gate and no threshold; it is listed here so that a reviewer can reject the reading.

**F4.** Evidence: `[ ! -e "$FINAL" ]` at start-up only; `mv "$STAGE" "$FINAL"`; `tar -czf "$TARBALL"`. Reproduction: runner lines with a pre-existing `FINAL` moved the stage to `FINAL/<stage>` with exit 0; end to end, a directory created 15 s into a smoke run. Impact: a validated bundle silently gains foreign content and its tarball/checksum are replaced. Verification: after the fix the end-to-end race exits 11, the intruding result is untouched and the staging directory is preserved; normal publication still verifies.

**F5.** Evidence: E1 `*.meta` record `commit_at_configure` (`fe4c5c4`, `c8354f3`), captured by CMake `execute_process` at configure time; the final E1 data were run at `24f5391`, after the allocation-hook fix `84706f9`, and the superseded (defective-hook) data carry the *same* SHAs. MEASURED: final data, N=64 glibc regimes, about 260 ns/call; superseded about 1,370 ns/call, the known signature of the defect. So the published E1 data used the fixed hook and E1's conclusions stand, but the recorded SHA identifies neither binary uniquely. E2 is unaffected (configure SHA = run commit, and the harness code is identical between its two data commits). R1 is unaffected: `run_replication.sh` deletes its build directory and re-configures from a clean tree. Proposal: an erratum note in `e1-results/README.md`; record a build-time SHA in future harnesses.

**F6.** Evidence: `cpu_reference.cpp`, the M7 `fusible(k)` check does not know about recompute-elided values. Reproduction (minimal, 9 ops): `X=A+C; out X; D=A+B; T=X+C; R=T+D; out R` under `{reuse:off, fuse:on, recompute:on}` returns `internal invariant violation: op 7 operand unbound`; the default policy is correct. Differential fuzzer: 2,000,000 random verified programs x 8 policies = 16,000,000 executions under ASan+UBSan: 0 wrong outputs, 0 input mutations, 0 sanitizer reports; errors only under the two policies combining fuse and recompute (about 1.6% and 0.4% of programs), always fail-closed. Impact: none on any experiment (E2 uses `{reuse, recompute}` without fusion; the default policy has `recompute=false`), but the internal seam is not total. Tested patch (not committed, runtime is out of scope): at the fusion call site, `slot && !(policy.recompute && elide[s.operations[i + 1].operands[1 - *slot].index()])`; with it the reproduction succeeds and 800,000 fuzzed executions give 0 errors under every policy.

**F7.** OBSERVED: no occurrence of `SHA256SUMS`, `resources.txt` or `MANIFEST` in the analyzer. The runner enforces all three at run time, so this is a defense-in-depth gap. RAM is not in the preregistered INCONCLUSIVE list, so enforcing it in the analysis would be a protocol decision, not a fix.

## 3. Scientific integrity (preregistration -> implementation -> raw data -> analyzer -> report)

- **Derived artifacts reproduce exactly** (MEASURED): every committed derived file of E1 (`gcc_main`, `clang_main`, `gcc_fresh_default`, `derived_combined_with_gcc_main`) and E2 (`gcc_main`, `clang_main`, including the post hoc S4 tables) regenerates byte-for-byte from the committed raw CSVs with the committed scripts.
- **Raw data integrity** (MEASURED, independent script): E2 43,740 rows per compiler, 324 cells x 135 samples, 0 duplicates, 0 incorrect, 0 arena fallbacks, 0 overflows. E1 64,575 rows per compiler and 12,915 supplementary, 0 duplicates, 0 incorrect.
- **Order of evidence** (OBSERVED): each preregistration commit is an ancestor of the harness that produced its data, and that harness is an ancestor of the commit that added the data (E1 `ded583b`, E2 `187e0df`).
- **R1 analyzer vs preregistration** (OBSERVED): gate A (S4 regret > 1.05 with bootstrap corroboration, >= 20 %), gate B (p95 >= 1.15), gate C (pure regime flip, both margins >= 5 %, stable in all replicates), "holds on a machine only if for every installed compiler", the transfer criterion and the verdict order match sections 4-7. Deviations: F3 (now fixed) and F7.
- **Known and disclosed** (OBSERVED in the reports): E2 gate C counts pairs combinatorially (the post hoc C_ADJACENT reduces 80/127 pairs to 7/10 boundaries); S2-S4 are oracle-static by design; one KVM guest.
- **Confounder not separable in E2 or R1** (HYPOTHESIS): every N in the matrix is a power of two (64 ... 16 Mi). Cache-set aliasing and 4 KiB-offset effects therefore cannot be separated from size; E2's non-monotone points (64 Ki, 4 Mi) may be layout effects. The frozen R1 protocol cannot address this; a later experiment should include non-power-of-two sizes.

## 4. What was not verified

Any physical primary run (no eligible host here); the primary branches of the swap and publication gates inside a full physical run (executed verbatim in isolation instead); real ARM64; PMU counters (none exposed); executor behaviour for N > 70 in the fuzzer (sizes 1-70 only); and the facts about the AMD FX-6300 smoke host, which were reported to me, not reproduced.
