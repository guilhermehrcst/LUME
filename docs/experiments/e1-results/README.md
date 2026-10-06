# E1 results

Raw data are exactly as written by `lume_e1_bench` (`experiments/e1-is-planning-necessary/`); nothing was edited or filtered. `derived/` is produced by `experiments/e1-is-planning-necessary/analyze.py` from the raw files in the same directory.

| Directory | What | Pre-registered? |
| --- | --- | --- |
| `gcc_main/` | the full preregistered matrix: GCC 13.3, 3 replicates, regimes `glibc_default`, `glibc_t1`, `glibc_t2`, `arena_cold`, `arena_warm` (64,575 samples) | yes |
| `clang_main/` | the same matrix with Clang 18.1.3, 3 replicates (64,575 samples); reported separately, never pooled | permitted by the preregistration |
| `gcc_fresh_default/` | **post hoc, supplementary**: regime `glibc_default_fresh` (one process per program x N cell), 3 replicates (12,915 samples); `derived/` analyses it alone, `derived_combined_with_gcc_main/` together with `gcc_main` | no |
| `superseded_v1_hook_scan/` | the first execution of all three sets above, produced with a defective allocation hook (see its README); kept unedited, do not use its small-N glibc timings | superseded |

Files per directory: `raw_r<replicate>_<process>.csv` (one row per timed sample), `raw_*.csv.meta` (environment of that process), `plans_*.csv` (the distinct plans and the executor's own counts per program x N), `log_*.txt` (progress and failures; empty of failures), `run_meta.txt`, `DONE`. In `raw_*.csv.meta`, `commit_at_configure` is the commit seen when CMake last configured the build directory and can be older than the code that ran; `run_meta.txt` records the repository commit at run time.

Raw CSV columns: `replicate, program, dtype, n, regime, plan, rep, order_pos, k, ns_per_call, minflt_per_call, majflt_per_call, utime_ns_per_call, stime_ns_per_call, result_allocs_per_call, result_alloc_bytes_per_call, arena_allocs_per_call, arena_fallbacks, table_overflows, peak_live_bytes_interval, fresh, in_place, fused, copies, moves, correct`. `plan` is P5, P6, P7 (the canonical label of a group of policies with identical execution, e.g. `P6` for `P6=P7`) or `FIXED_TWIN` (a second series of the fixed plan, used only to measure noise). `peak_live_bytes_interval` is the peak of result-sized bytes held during the interval of K calls (K results are kept for checking); the per-call peak is `dry_peak_live_bytes` in `plans_*.csv`.
