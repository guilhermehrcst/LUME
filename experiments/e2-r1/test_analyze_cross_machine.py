#!/usr/bin/env python3
"""Self-test of analyze_cross_machine.py with synthetic bundles whose correct verdict is known
by construction, plus validation of the real committed E2 VM data. Not performance data."""
import argparse
import math
import os
import random
import shutil
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze_cross_machine as acm  # noqa: E402

REPS, REPLS = 3, 3
FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def h(*parts):
    return zlib.crc32("|".join(map(str, parts)).encode())


def times(seed, dtype, k, n, regime, flips, flip_set):
    """Return (t_M, t_R) medians in ns for a synthetic cell."""
    base = 200 + n * (k + 1) * 0.3
    if flips and (dtype, k, n) in flip_set:
        # pure regime flip: M wins by 15% in arena_warm, R wins by 15% in glibc_default_fresh, tie in arena_cold
        r = {"arena_warm": 0.85, "arena_cold": 1.0, "glibc_default_fresh": 1.15}[regime]
    else:
        r = 0.88 if h(seed, dtype, k, n) % 2 == 0 else 1.12   # static size-dependent winner, same in every regime
    return base * r ** 0.5, base / r ** 0.5


def write_dataset(d, seed, flips=False, bad_correct=False, twin_slow=1.0):
    os.makedirs(d)
    rng = random.Random(h(seed, "noise"))
    flip_set = {("f32", 2, n) for n in acm.SIZES[1:7]} | {("i32", 3, n) for n in acm.SIZES[1:7]}
    cols = "replicate,dtype,fanout,n,regime,strategy,rep,ns_per_call,correct,arena_fallbacks,table_overflows"
    for rep in range(1, REPLS + 1):
        for part, regs in (("arena", ["arena_warm", "arena_cold"]), ("fresh", ["glibc_default_fresh"])):
            with open(os.path.join(d, f"raw_r{rep}_{part}.csv"), "w") as fh:
                fh.write(cols + "\n")
                for dt in acm.DTYPES:
                    for k in acm.KS:
                        for n in acm.SIZES:
                            for rg in regs:
                                tm, tr = times(seed, dt, k, n, rg, flips, flip_set)
                                for i in range(REPS):
                                    for s, t in (("M", tm), ("R", tr), ("M_TWIN", tm * twin_slow)):
                                        ns = t * math.exp(rng.gauss(0, 0.004))
                                        ok = 0 if (bad_correct and rep == 1 and i == 0 and dt == "f32" and k == 1 and n == 64 and s == "R") else 1
                                        fh.write(f"{rep},{dt},{k},{n},{rg},{s},{i},{ns:.2f},{ok},0,0\n")


def make_machine(root, mid, arch, compilers, physical=True, swap_out=0):
    md = os.path.join(root, mid)
    os.makedirs(md, exist_ok=True)
    open(os.path.join(md, "environment.txt"), "w").write(f"arch={arch}\ncpu_model=synthetic\nkernel=0\nswap_total=0 kB\n")
    open(os.path.join(md, "physical.txt"), "w").write("PHYSICAL=yes\n" if physical else "PHYSICAL=no\n")
    open(os.path.join(md, "swap_activity.txt"), "w").write(f"pswpin_delta=0\npswpout_delta={swap_out}\n")
    return md


def build(root, spec):
    """spec: list of (machine, arch, {compiler: dict(seed=.., flips=..)}, extra kwargs)."""
    for mid, arch, comps, extra in spec:
        md = make_machine(root, mid, arch, comps, physical=extra.get("physical", True), swap_out=extra.get("swap_out", 0))
        for comp, kw in comps.items():
            write_dataset(os.path.join(md, comp), **kw)


def analyze(root):
    args = argparse.Namespace(root=root, e2_vm_dir=None, replicates=REPLS, reps=REPS)
    machines, excluded, results, transfers, verdict, reasons, audits = acm.run_analysis(args)
    return verdict, reasons, results, transfers, excluded


def scenario(name, spec, expect, **extra):
    root = tempfile.mkdtemp(prefix="r1test_")
    try:
        build(root, spec)
        verdict, reasons, results, transfers, excluded = analyze(root)
        check(verdict == expect, f"{name}: verdict {verdict!r} == {expect!r}   ({'; '.join(reasons)[:150]})")
        return verdict, reasons, results, transfers, excluded
    finally:
        shutil.rmtree(root, ignore_errors=True)


def codegen_parser_fixture():
    """codegen_report.py on a self-contained kernel fixture compiled for x86-64 and (if clang/llvm-objdump can) aarch64."""
    import shutil as sh
    import subprocess
    clang, llvm_od = sh.which("clang++"), sh.which("llvm-objdump-18") or sh.which("llvm-objdump")
    fx = os.path.join(HERE, "fixtures", "codegen_parser_fixture.cpp")
    rep = os.path.join(HERE, "..", "e2-materialize-vs-recompute", "codegen_report.py")
    if not (clang and llvm_od):
        print("skip codegen parser fixture (clang or llvm-objdump missing)")
        return
    for target in ("x86_64-linux-gnu", "aarch64-linux-gnu"):
        with tempfile.TemporaryDirectory() as t:
            o, dis = os.path.join(t, "f.o"), os.path.join(t, "f.dis")
            subprocess.run([clang, f"--target={target}", "-O3", "-DNDEBUG", "-c", fx, "-o", o], check=True)
            text = subprocess.run([llvm_od, "-d", "-C", "--no-show-raw-insn"] + (["--x86-asm-syntax=intel"] if target.startswith("x86") else []) + [o],
                                  check=True, capture_output=True, text=True).stdout
            open(dis, "w").write(text)
            out = subprocess.run([sys.executable, rep, dis, target], check=True, capture_output=True, text=True).stdout
            rows = [l for l in out.splitlines() if l.startswith("| fixture_")]
            check(len(rows) >= 3 and all(int(r.split("|")[3]) >= 2 for r in rows), f"codegen parser finds the vector loops of the fixture on {target} ({len(rows)} loops)")


def main():
    both = lambda **kw: {"gcc": dict(**kw), "clang": dict(**kw)}
    # T1 portable: same static pattern on both machines, no regime effect
    v, r, res, tr, _ = scenario("portable", [("X1", "x86_64", both(seed=1), {}), ("A1", "aarch64", both(seed=1), {})],
                                "PORTABLE STATIC POLICY SUFFICES")
    check(all(not x["gate_A"] and not x["gate_B"] and not x["gate_C"] for x in res.values()), "portable: all runtime gates fail")
    check(all(not t["fail"] for t in tr.values() if t), "portable: every transfer passes")
    # T2 machine-calibrated: different static pattern on ARM
    v, r, res, tr, _ = scenario("machine-calibrated", [("X1", "x86_64", both(seed=1), {}), ("A1", "aarch64", both(seed=2), {})],
                                "MACHINE-CALIBRATED STATIC POLICY")
    check(all(not x["gate_A"] and not x["gate_B"] and not x["gate_C"] for x in res.values()), "machine-calibrated: runtime gates fail (S4 is per machine)")
    check(any(t["fail"] for t in tr.values() if t), "machine-calibrated: a transfer fails")
    # T3 regime-sensitive: pure regime flips on X for both compilers
    v, r, res, tr, _ = scenario("regime-sensitive", [("X1", "x86_64", both(seed=1, flips=True), {}), ("A1", "aarch64", both(seed=1), {})],
                                "REGIME-SENSITIVE PLANNING SURVIVES")
    check(len(res[("X1", "gcc")]["pure_flips"]) > 0 and len(res[("A1", "gcc")]["pure_flips"]) == 0, "regime-sensitive: flips only where planted")
    # T3b the signal in one compiler only is not enough
    v, r, res, tr, _ = scenario("single-compiler signal",
                                [("X1", "x86_64", {"gcc": dict(seed=1, flips=True), "clang": dict(seed=1)}, {}), ("A1", "aarch64", both(seed=1), {})],
                                "PORTABLE STATIC POLICY SUFFICES")
    check(any("only" in x for x in r), "single-compiler signal: reported as unconfirmed")
    # INCONCLUSIVE cases
    scenario("correctness failure", [("X1", "x86_64", {"gcc": dict(seed=1, bad_correct=True), "clang": dict(seed=1)}, {}), ("A1", "aarch64", both(seed=1), {})], "INCONCLUSIVE")
    scenario("missing ARM machine", [("X1", "x86_64", both(seed=1), {})], "INCONCLUSIVE")
    v, r, res, tr, ex = scenario("non-physical ARM excluded", [("X1", "x86_64", both(seed=1), {}), ("A1", "aarch64", both(seed=1), {"physical": False})], "INCONCLUSIVE")
    check(any(m == "A1" for m, _ in ex), "non-physical host listed as excluded")
    scenario("smoke bundle excluded", [("X1", "x86_64", both(seed=1), {}), ("SMOKE-A1", "aarch64", both(seed=1), {})], "INCONCLUSIVE")
    scenario("excess twin noise", [("X1", "x86_64", {"gcc": dict(seed=1, twin_slow=1.2), "clang": dict(seed=1)}, {}), ("A1", "aarch64", both(seed=1), {})], "INCONCLUSIVE")
    scenario("swap activity", [("X1", "x86_64", both(seed=1), {"swap_out": 7}), ("A1", "aarch64", both(seed=1), {})], "INCONCLUSIVE")
    # the validator on the real committed E2 VM data (no analysis claim)
    e2 = os.path.join(HERE, "..", "..", "docs", "experiments", "e2-results")
    for sub in ("gcc_main", "clang_main"):
        d = os.path.join(e2, sub)
        if os.path.isdir(d):
            pr, info = acm.validate_dir(d, replicates=3, reps=15)
            check(not pr and info["rows"] == 43740, f"validator accepts committed E2 data {sub}: {info}")
    codegen_parser_fixture()
    print("RESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
