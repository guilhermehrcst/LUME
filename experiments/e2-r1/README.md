# E2-R1: cross-machine physical replication

Preregistration (frozen): [`docs/experiments/e2-r1-cross-machine-replication.md`](../../docs/experiments/e2-r1-cross-machine-replication.md).
Question: does E2's size-dependent Materialize-vs-Recompute choice reproduce on **physical** machines, and is machine identity itself needed to choose the best static action?

**Do not run the primary replication on a VM, container, cloud VM or CI runner.** `check_physical_host.sh` refuses, and the analysis refuses a bundle that is not marked `PHYSICAL=yes`. Primary data are valid only from a physical x86-64 host **and** a physical ARM64 host.

## What you need on each host

Linux, glibc, `git`, `cmake >= 3.20`, GCC and/or Clang (both preferred; exact versions are recorded), `python3`, `binutils` (`objdump`), at least 8 GiB of RAM available (`MemAvailable` >= 8,388,608 kB in `/proc/meminfo`, whose "kB" are 1024-byte units; swap does not count), no swap activity. No `sudo` is needed to run; counters use `perf_event_open` in user space (works with `perf_event_paranoid <= 2`; if the PMU is not exposed the secondary characterization records the events as unsupported and the primary timing is unaffected). Expect roughly 1.5-2.5 hours per compiler. Leave the machine otherwise idle; do not pin CPUs (the E2 protocol did not). Optionally set the performance governor first; whatever the governor and boost state are, they are recorded.

```
# Debian/Ubuntu example
sudo apt-get install -y build-essential cmake clang python3 git binutils
```

## Run (exact commands)

Physical x86-64 host (Machine X):

```
git clone https://github.com/guilhermehrcst/LUME.git && cd LUME
git checkout lume/e2-r1-cross-machine-replication-v01      # or main once this branch is merged
experiments/e2-r1/check_physical_host.sh                    # must end with PHYSICAL=yes
experiments/e2-r1/run_replication.sh x86-<cpu>-<nn>          # e.g. x86-ryzen7-5800x-01
```

Physical ARM64 host (Machine A), the same on an `aarch64` Linux machine:

```
git clone https://github.com/guilhermehrcst/LUME.git && cd LUME
git checkout lume/e2-r1-cross-machine-replication-v01
experiments/e2-r1/check_physical_host.sh
experiments/e2-r1/run_replication.sh arm64-<cpu>-<nn>        # e.g. arm64-graviton3-metal-01
```

`run_replication.sh` refuses a dirty working tree, less than 8 GiB `MemAvailable` (checked by `check_resources.sh` before any build; a host that cannot meet it, even when idle, cannot produce R1 primary data), a non-physical host, an existing result, non-primary flags (`-march=native`, `-ffast-math`, a non-`-O3 -DNDEBUG` build), failing unit tests, any correctness mismatch, any invalid cell, an incomplete matrix, and swap activity during the run (the staging directory is kept for diagnosis, nothing is published; the analysis independently marks swap activity INCONCLUSIVE). Options: `--no-pmu` (skip the secondary PMU characterization), `--replicates N` (default 3; the preregistered value is 3).

## Output bundle

`docs/experiments/e2-r1-results/<machine-id>/` (created atomically only after validation, never overwritten), plus `<machine-id>.tar.gz` and `<machine-id>.tar.gz.sha256` next to it:

```
environment.txt  physical.txt  swap_activity.txt  MANIFEST.txt  SHA256SUMS  flags_<cc>.txt  cmake_/build_/ctest_<cc>.log
gcc/    raw_r{1..3}_{arena,fresh}.csv  strategies_r*_*.csv  run_meta.txt  log_*  DONE      (primary timing, 43,740 samples)
clang/  (same)
pmu_gcc/, pmu_clang/   probe_<cc>.csv  pmu_<cc>_{A,B,C}.csv  pmu_log_<cc>.txt              (secondary, separate)
codegen/   <cc>-<arch>.{report.md,vec.txt,dis,meta}                                          (secondary)
```

`run_meta.txt` contains `uname -a`, which includes the host name; edit it before publishing if that matters (the checksums will then need regenerating, so record the edit in the commit message).

## Returning the data

Either send `<machine-id>.tar.gz` and its `.sha256`, or commit the directory on a branch (`lume/e2-r1-data-<machine-id>`). Import on the analysis machine:

```
cd docs/experiments/e2-r1-results
sha256sum -c <machine-id>.tar.gz.sha256 && tar -xzf <machine-id>.tar.gz
(cd <machine-id> && sha256sum -c SHA256SUMS)
```

## Analysis (after both machines exist)

```
python3 experiments/e2-r1/analyze_cross_machine.py docs/experiments/e2-r1-results \
        --e2-vm-dir docs/experiments/e2-results --out docs/experiments/e2-r1-results/analysis.md --json docs/experiments/e2-r1-results/analysis.json
```

The script validates every bundle, evaluates gates A, B and C per machine and compiler, the cross-machine transfer, and prints exactly one verdict from the preregistered set. Run `python3 experiments/e2-r1/test_analyze_cross_machine.py` (also a ctest) to see the verdict logic exercised on synthetic bundles with known answers.

## Tooling smoke test (NOT data)

```
LUME_R1_ALLOW_VIRTUAL=1 experiments/e2-r1/run_replication.sh SMOKE-<name> --smoke
```

runs a tiny matrix on any host, writes a `SMOKE-*` bundle with `physical=false` (git-ignored, excluded from the analysis). Delete it afterwards.

## Status of this tooling

- **Physical x86-64 (tooling validation only).** Exercised on bare-metal Linux on an AMD FX-6300 (Gigabyte GA-78LMT-USB3 6.0), where `check_physical_host.sh` reports `PHYSICAL=yes`. Validated there with smoke runs: the physical-host detection path, environment capture, the GCC and Clang build and unit-test paths, the primary-matrix smoke path with correctness checks and result validation, the codegen path, and bundle creation with checksums. The PMU path was exercised: with `perf_event_paranoid=2` all 14 preregistered events (cycles, instructions, branches, branch-misses, cache-references, cache-misses, L1-dcache-loads, L1-dcache-load-misses, LLC-loads, LLC-load-misses, dTLB-load-misses, page-faults, minor-faults, major-faults) opened successfully (`supported=1`, `errno=0`) for both GCC and Clang builds.
- **No E2-R1 primary data were collected from that host.** It has about 7.2 GiB of usable RAM in total, so it cannot satisfy the preregistered requirement of at least 8 GiB of RAM available even when idle; swap does not count. Its smoke bundles are tooling validation, not data, and are not in the repository. The tooling passed a physical smoke; the host did not qualify for R1.
- **x86-64 KVM guest (earlier).** Host refusal, smoke chain with both compilers, unit tests, the validator on the committed E2 data, the verdict logic on synthetic bundles, and the AArch64 disassembly parser on a cross-compiled kernel fixture.
- **Not yet exercised on real ARM64 hardware.** The full ARM64 build and run paths of the harness and the PMU binary are unverified until the first ARM64 run; the cross-compiled parser fixture is not an ARM64 execution.
