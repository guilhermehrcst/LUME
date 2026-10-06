#!/usr/bin/env python3
"""E2-R1 cross-machine analysis. Implements sections 4-7 of
docs/experiments/e2-r1-cross-machine-replication.md. Pure standard library; the statistics
(decisive winner, regret, paired bootstrap, per-replicate stability) are the code of
experiments/e2-materialize-vs-recompute/e2_audit.py, which reproduces the preregistered E2 numbers.

usage:
  analyze_cross_machine.py <results-root> [--e2-vm-dir <dir>] [--out report.md] [--json summary.json]
                           [--replicates 3] [--reps 15]
  analyze_cross_machine.py --validate-bundle-dir <dir> (--replicates N | --smoke)

<results-root>/<machine-id>/ contains environment.txt, physical.txt, swap_activity.txt and one
sub-directory per compiler (gcc/, clang/) with the raw_*.csv files of run_e2.sh.
Machines whose id starts with SMOKE- or whose physical.txt is not PHYSICAL=yes are listed and excluded.
"""
import argparse
import collections
import csv
import glob
import json
import math
import os
import random
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "e2-materialize-vs-recompute"))
import e2_audit  # noqa: E402  (shared, validated statistics)

DTYPES = ["f32", "i32"]
KS = [1, 2, 3, 4, 6, 8]
SIZES = [64, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216]
REGIMES = ["arena_warm", "arena_cold", "glibc_default_fresh"]
STRATS = ["M", "R", "M_TWIN"]
COMPILERS = ["gcc", "clang"]
NOISE_LIMIT = 1.10


def p95(v):
    return e2_audit.p95(v)


# ----------------------------------------------------------------------------- validation

def validate_dir(d, replicates=None, reps=15, smoke=False):
    """Return (problems, info). Fail-closed checks on one compiler directory."""
    problems, info = [], {}
    raws = sorted(glob.glob(os.path.join(d, "raw_*.csv")))
    if not raws:
        return ["no raw_*.csv in " + d], info
    rows = 0
    seen = collections.defaultdict(set)  # cell -> set of (strategy, replicate, rep)
    for f in raws:
        for r in csv.DictReader(open(f, newline="")):
            rows += 1
            if r["correct"] != "1":
                problems.append(f"correctness failure: {r['dtype']} k={r['fanout']} n={r['n']} {r['regime']} {r['strategy']}")
            if int(r["arena_fallbacks"]) != 0:
                problems.append(f"arena fallback in {r['dtype']} k={r['fanout']} n={r['n']} {r['regime']}")
            if int(r["table_overflows"]) != 0:
                problems.append(f"hook table overflow in {r['dtype']} k={r['fanout']} n={r['n']} {r['regime']}")
            seen[(r["dtype"], int(r["fanout"]), int(r["n"]), r["regime"])].add((r["strategy"], r["replicate"], int(r["rep"])))
    info["rows"], info["cells"] = rows, len(seen)
    if not smoke:
        want_cells = len(DTYPES) * len(KS) * len(SIZES) * len(REGIMES)
        if len(seen) != want_cells:
            problems.append(f"incomplete matrix: {len(seen)} cells, expected {want_cells}")
        units = replicates * reps if replicates else None
        for c, s in seen.items():
            for st_ in STRATS:
                n = sum(1 for x in s if x[0] == st_)
                if units is not None and n != units:
                    problems.append(f"cell {c}: {st_} has {n} samples, expected {units}")
                    break
        if replicates and rows != want_cells * len(STRATS) * replicates * reps:
            problems.append(f"row count {rows} != expected {want_cells * len(STRATS) * replicates * reps}")
    return sorted(set(problems))[:50], info


def twin_p95(A, cells):
    v = []
    for c in cells:
        m = st.median(A.S[c]["M"].values())
        t = st.median(A.S[c]["M_TWIN"].values())
        v.append(max(m, t) / min(m, t))
    return p95(v) if v else float("nan")


# ----------------------------------------------------------------------------- per machine/compiler analysis

def groups_by_n(cells):
    g = collections.defaultdict(list)
    for c in cells:
        g[(c[0], c[1], c[2])].append(c)
    return g


def s4_table(A, cells):
    return {g: A.best_action(cs) for g, cs in groups_by_n(cells).items()}


def pure_regime_flips(A, cells, s4_groups):
    flips = []
    for g, cs in s4_groups.items():
        for i, c1 in enumerate(cs):
            for c2 in cs[i + 1:]:
                w1, w2 = A.decisive(c1), A.decisive(c2)
                if w1 is None or w2 is None or w1 == w2:
                    continue
                alt1, alt2 = ("R" if w1 == "M" else "M"), ("R" if w2 == "M" else "M")
                m1 = A.stat[c1][alt1] / A.stat[c1][w1]
                m2 = A.stat[c2][alt2] / A.stat[c2][w2]
                if m1 < 1.05 or m2 < 1.05:
                    continue
                if not A.stable(c1, w1, c2, w2):
                    continue
                flips.append({"group": g, "regimes": (c1[3], c2[3]), "winners": (w1, w2), "wrong_action_cost": (m1, m2)})
    return flips


def analyze_one(d):
    A = e2_audit.Audit(e2_audit.load(d))
    cells = A.all_cells
    tab = s4_table(A, cells)
    s4_reg = [A.regret(c, tab[(c[0], c[1], c[2])]) for c in cells]
    rng = random.Random(e2_audit.SEED)
    opp = sum(A.corroborated(c, tab[(c[0], c[1], c[2])], rng) for c in cells)
    flips = pure_regime_flips(A, cells, groups_by_n(cells))
    # secondary: S4-CV and S3 / byte-rule regret as in E2
    full = A.run(cells)
    out = {
        "cells": len(cells), "units": len(set(A.S[cells[0]]["M"])),
        "twin_p95": twin_p95(A, cells),
        "S4": A.summ(s4_reg), "S4_opp": opp, "S4_opp_rate": opp / len(cells),
        "gate_A": opp / len(cells) >= 0.20, "gate_B": p95(s4_reg) >= 1.15, "gate_C": len(flips) > 0,
        "pure_flips": flips,
        "S4cv": full["S4cv"], "S4cv_opp_rate": full["S4cv_opp"] / max(1, full["S4cv"]["n"]),
        "S3": full["S3"], "C_STRICT_pairs": full["strict"]["pairs"], "C_ADJACENT_pairs": full["adj"]["pairs"],
    }
    return A, tab, out


def transfer(table, A_target):
    reg = []
    for c in A_target.all_cells:
        act = table.get((c[0], c[1], c[2]))
        if act is None:
            return None
        reg.append(A_target.regret(c, act))
    s = A_target.summ(reg)
    s["fail"] = s["gt105"] >= 0.20 or s["p95"] >= 1.10
    return s


# ----------------------------------------------------------------------------- bundles

def read_kv(path):
    d = {}
    if os.path.exists(path):
        for line in open(path):
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                d[k.strip()] = v.strip()
    return d


def discover(root):
    machines, excluded = {}, []
    for md in sorted(glob.glob(os.path.join(root, "*"))):
        if not os.path.isdir(md):
            continue
        mid = os.path.basename(md)
        env = read_kv(os.path.join(md, "environment.txt"))
        phys = open(os.path.join(md, "physical.txt")).read() if os.path.exists(os.path.join(md, "physical.txt")) else ""
        if mid.startswith("SMOKE-") or "PHYSICAL=yes" not in phys:
            excluded.append((mid, "SMOKE bundle" if mid.startswith("SMOKE-") else "not a verified physical host (physical.txt missing or PHYSICAL!=yes)"))
            continue
        comp = {c: os.path.join(md, c) for c in COMPILERS if os.path.isdir(os.path.join(md, c)) and glob.glob(os.path.join(md, c, "raw_*.csv"))}
        machines[mid] = {"dir": md, "arch": env.get("arch", "unknown"), "env": env, "compilers": comp,
                         "swap": read_kv(os.path.join(md, "swap_activity.txt"))}
    return machines, excluded


def decide(machines, results, transfers, problems, reps_ok):
    """Return (verdict, reasons) following section 7 of the preregistration."""
    reasons = []
    arches = {m["arch"] for m in machines.values()}
    if "x86_64" not in arches or not ({"aarch64", "arm64"} & arches):
        reasons.append(f"required physical machine missing (have architectures: {sorted(arches) or 'none'}; need x86_64 and aarch64)")
    reasons += problems
    for (mid, comp), r in results.items():
        if r["twin_p95"] >= NOISE_LIMIT:
            reasons.append(f"{mid}/{comp}: twin-series noise p95 {r['twin_p95']:.3f} >= {NOISE_LIMIT}")
    for mid, m in machines.items():
        if m["swap"].get("pswpout_delta", "0") not in ("0", "") or m["swap"].get("pswpin_delta", "0") not in ("0", ""):
            reasons.append(f"{mid}: swap activity during the run ({m['swap']})")
    if reasons:
        return "INCONCLUSIVE", reasons
    # runtime gates: per machine, must hold for every installed compiler
    holds = {}
    unconfirmed = []
    for mid, m in machines.items():
        comps = [c for c in m["compilers"]]
        for gate in ("gate_A", "gate_B", "gate_C"):
            per = [results[(mid, c)][gate] for c in comps]
            if per and all(per):
                holds.setdefault(mid, []).append(gate)
            elif any(per):
                unconfirmed.append(f"{mid}: {gate} holds for {[c for c, v in zip(comps, per) if v]} only")
    if holds:
        return "REGIME-SENSITIVE PLANNING SURVIVES", [f"{mid}: {g} holds for every installed compiler" for mid, g in holds.items()] + unconfirmed
    out = list(unconfirmed)
    failing = [(k, v) for k, v in transfers.items() if v is not None and v["fail"] and not k[0].startswith("VM")]
    if failing:
        return "MACHINE-CALIBRATED STATIC POLICY", out + [f"transfer {s}->{t} [{c}] fails: share>1.05 {v['gt105']:.3f}, p95 {v['p95']:.3f}" for (s, t, c), v in failing]
    return "PORTABLE STATIC POLICY SUFFICES", out + ["all runtime gates fail and every gated cross-machine transfer passes"]


def run_analysis(args):
    machines, excluded = discover(args.root)
    problems, results, tables, audits = [], {}, {}, {}
    for mid, m in machines.items():
        if not m["compilers"]:
            problems.append(f"{mid}: no compiler data")
        for comp, d in m["compilers"].items():
            pr, info = validate_dir(d, replicates=args.replicates, reps=args.reps)
            problems += [f"{mid}/{comp}: {p}" for p in pr]
            if pr:
                continue
            A, tab, out = analyze_one(d)
            results[(mid, comp)], tables[(mid, comp)], audits[(mid, comp)] = out, tab, A
    transfers = {}
    ids = sorted(machines)
    for s in ids:
        for t in ids:
            if s == t or machines[s]["arch"] == machines[t]["arch"]:
                continue
            for comp in COMPILERS:
                if (s, comp) in tables and (t, comp) in audits:
                    transfers[(s, t, comp)] = transfer(tables[(s, comp)], audits[(t, comp)])
    # secondary: the E2 VM tables applied to the physical machines (never gating)
    vm = {}
    if args.e2_vm_dir:
        for comp, sub in (("gcc", "gcc_main"), ("clang", "clang_main")):
            dd = os.path.join(args.e2_vm_dir, sub)
            if os.path.isdir(dd):
                Av = e2_audit.Audit(e2_audit.load(dd))
                vm[comp] = s4_table(Av, Av.all_cells)
        for (t, comp), At in audits.items():
            if comp in vm:
                transfers[("VM-E2", t, comp)] = transfer(vm[comp], At)
    verdict, reasons = decide(machines, results, transfers, problems, True)
    return machines, excluded, results, transfers, verdict, reasons, audits


def fmt_report(machines, excluded, results, transfers, verdict, reasons, audits):
    L = ["# E2-R1 cross-machine analysis (generated by analyze_cross_machine.py)\n", f"## Verdict: **{verdict}**\n"]
    L += [f"- {r}" for r in reasons]
    if verdict == "REGIME-SENSITIVE PLANNING SURVIVES":
        pass
    elif verdict in ("MACHINE-CALIBRATED STATIC POLICY", "PORTABLE STATIC POLICY SUFFICES"):
        L.append("- RUNTIME REGIME-SENSITIVE PLANNER FAILS FOR THIS SPACE (gates A, B and C all fail on every physical machine).")
    L.append("\n## Hosts\n")
    for mid, m in machines.items():
        L.append(f"- `{mid}`: arch {m['arch']}, compilers {sorted(m['compilers'])}, cpu `{m['env'].get('cpu_model', m['env'].get('cpu_model_lscpu', '?'))}`, kernel `{m['env'].get('kernel', '?')}`")
    for mid, why in excluded:
        L.append(f"- `{mid}`: EXCLUDED ({why})")
    L.append("\n## Per machine and compiler (primary baseline S4 = best fixed action per (dtype,k,N), constant across regimes)\n")
    L.append("| machine | compiler | cells | units | twin p95 | S4 median / p95 / max | A: opp. rate | B: p95 | C: pure flips | S4-CV p95 / opp. rate | S3 p95 |")
    L.append("| --- | --- | ---: | ---: | ---: | --- | --- | --- | ---: | --- | ---: |")
    for (mid, comp), r in sorted(results.items()):
        L.append(f"| {mid} | {comp} | {r['cells']} | {r['units']} | {r['twin_p95']:.3f} | {r['S4']['median']:.3f} / {r['S4']['p95']:.3f} / {r['S4']['max']:.3f} | "
                 f"{r['S4_opp_rate']:.3f} ({'holds' if r['gate_A'] else 'fails'}) | {r['S4']['p95']:.3f} ({'holds' if r['gate_B'] else 'fails'}) | "
                 f"{len(r['pure_flips'])} ({'holds' if r['gate_C'] else 'fails'}) | {r['S4cv']['p95']:.3f} / {r['S4cv_opp_rate']:.3f} | {r['S3']['p95']:.3f} |")
    L.append("\n### Pure regime flips (gate C)\n")
    for (mid, comp), r in sorted(results.items()):
        for f in r["pure_flips"]:
            L.append(f"- {mid}/{comp}: dtype={f['group'][0]} k={f['group'][1]} N={f['group'][2]} {f['regimes']} winners {f['winners']} cost of wrong action {f['wrong_action_cost'][0]:.3f} / {f['wrong_action_cost'][1]:.3f}")
    L.append("\n## Cross-machine static transfer (S4 from source applied unchanged to target; failure: >=20% of cells with regret >1.05, or p95 >= 1.10)\n")
    L.append("| source | target | compiler | median | p95 | max | share > 1.05 | failure | gating |")
    L.append("| --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |")
    for (s, t, c), v in sorted(transfers.items()):
        if v is None:
            continue
        L.append(f"| {s} | {t} | {c} | {v['median']:.3f} | {v['p95']:.3f} | {v['max']:.3f} | {v['gt105']:.3f} | {'FAIL' if v['fail'] else 'ok'} | {'no (VM table, secondary)' if s.startswith('VM') else 'yes'} |")
    # agreement between machines
    keys = sorted({k[1] for k in audits})
    for comp in COMPILERS:
        ms = sorted(m for (m, c) in audits if c == comp)
        for i, a in enumerate(ms):
            for b in ms[i + 1:]:
                Aa, Ab = audits[(a, comp)], audits[(b, comp)]
                common = [c for c in Aa.all_cells if c in Ab.stat]
                agree = sum((Aa.stat[c]["M"] <= Aa.stat[c]["R"]) == (Ab.stat[c]["M"] <= Ab.stat[c]["R"]) for c in common)
                L.append(f"\nWinner agreement {a} vs {b} [{comp}]: {agree} / {len(common)} cells")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?")
    ap.add_argument("--e2-vm-dir")
    ap.add_argument("--out")
    ap.add_argument("--json")
    ap.add_argument("--replicates", type=int, default=3)
    ap.add_argument("--reps", type=int, default=15)
    ap.add_argument("--validate-bundle-dir")
    ap.add_argument("--smoke", action="store_true")
    args = ap.parse_args()
    if args.validate_bundle_dir:
        pr, info = validate_dir(args.validate_bundle_dir, replicates=None if args.smoke else args.replicates, reps=args.reps, smoke=args.smoke)
        print(f"validated {args.validate_bundle_dir}: {info}")
        if pr:
            print("VALIDATION FAILED:\n  " + "\n  ".join(pr))
            sys.exit(1)
        print("VALIDATION OK")
        return
    if not args.root:
        ap.error("results root required")
    machines, excluded, results, transfers, verdict, reasons, audits = run_analysis(args)
    rep = fmt_report(machines, excluded, results, transfers, verdict, reasons, audits)
    if args.out:
        open(args.out, "w").write(rep)
    if args.json:
        json.dump({"verdict": verdict, "reasons": reasons,
                   "results": {f"{m}/{c}": {k: v for k, v in r.items() if k != "pure_flips"} | {"pure_flips": len(r["pure_flips"])} for (m, c), r in results.items()},
                   "transfers": {f"{s}->{t}/{c}": v for (s, t, c), v in transfers.items() if v}}, open(args.json, "w"), indent=1, default=str)
    print(rep)


if __name__ == "__main__":
    main()
