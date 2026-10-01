# Measurement data

Files in this directory are raw output captured while the project was named
PXIR. They are evidence of what was measured at the time and are kept exactly
as captured. They were not edited in the rename to Lume.

What that means when you read them:

- Output keys and component names begin with `pxir_` (for example
  `pxir_execution_total`). The benchmarks now emit the same keys with a `lume_`
  prefix. The values and the measurement method are unchanged.
- The milestone write-ups in `docs/m*.md` cite these data files, so they keep
  the `pxir_` keys and the two historical branch names
  (`pxir/m3-single-write-result-v01`, `pxir/m6-last-use-inplace-reuse-v01`)
  exactly as they appear in the data and in git history.
- To reproduce a number with the current code, map `pxir_` to `lume_` in the key
  name. Nothing else changed.

The last commit under the old name is tagged `pre-lume-rename`.
