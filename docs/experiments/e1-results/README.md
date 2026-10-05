# E1 results

Raw data are exactly as written by `lume_e1_bench` (`experiments/e1-is-planning-necessary/`); nothing was edited or filtered. `derived/` is produced by `experiments/e1-is-planning-necessary/analyze.py` from the raw files in the same directory.

| Directory | What | Pre-registered? |
| --- | --- | --- |
| `gcc_main/` | the full preregistered matrix: GCC 13.3, 3 replicates, regimes `glibc_default`, `glibc_t1`, `glibc_t2`, `arena_cold`, `arena_warm` | yes |

Files per directory: `raw_r<replicate>_<process>.csv` (one row per timed sample), `raw_*.csv.meta` (environment of that process), `plans_*.csv` (the distinct plans and the executor's own counts per program x N), `log_*.txt` (progress and failures; empty of failures), `run_meta.txt`, `DONE`.

Raw CSV columns: `replicate, program, dtype, n, regime, plan, rep, order_pos, k, ns_per_call, minflt_per_call, majflt_per_call, utime_ns_per_call, stime_ns_per_call, result_allocs_per_call, result_alloc_bytes_per_call, arena_allocs_per_call, arena_fallbacks, table_overflows, peak_live_bytes_interval, fresh, in_place, fused, copies, moves, correct`. `plan` is P5, P6, P7 (the canonical label of a group of policies with identical execution, e.g. `P6` for `P6=P7`) or `FIXED_TWIN` (a second series of the fixed plan, used only to measure noise). `peak_live_bytes_interval` is the peak of result-sized bytes held during the interval of K calls (K results are kept for checking); the per-call peak is `dry_peak_live_bytes` in `plans_*.csv`.
