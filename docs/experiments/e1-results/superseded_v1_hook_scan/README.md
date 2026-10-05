# Superseded run v1 (kept, unedited)

This is the first full execution of the preregistered matrix (plus the Clang replicates and the post-hoc fresh-process default). It was produced with an allocation hook whose bookkeeping cost grew with the number of live result blocks (commit `fe4c5c4`; fixed in the commit that follows `625907a`). In the `glibc_*` regimes with K = 1000 (N < 4096) every free of a small pointer scanned about 1,000 entries, adding roughly 1 us per call to every plan in those cells. Large-N cells (K = 1) and the arena regimes were not affected.

It is retained, not deleted, because it was analysed before the flaw was found. Its verdict under the preregistered gates was the same as that of the corrected run (fails for the tested space), but its small-N `glibc_*` timings must not be used. The corrected run is in `../gcc_main/`, `../clang_main/` and `../gcc_fresh_default/`.
