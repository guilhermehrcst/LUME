#!/usr/bin/env python3
"""POST HOC (not preregistered): S4 = best fixed action per (dtype, k, N), constant
across memory regimes (oracle-static: chosen with the data). Also S5 = best action
per (dtype, k, N-class) with N-class in {<=16Ki, 64Ki-1Mi, >=4Mi}. Reads
<results-dir>/derived/cells.csv written by analyze.py; writes derived/posthoc_s4.md."""
import csv, math, statistics as st, sys, collections, os
d = sys.argv[1]
rows = list(csv.DictReader(open(os.path.join(d, "derived", "cells.csv"))))
def p95(v):
    s = sorted(v); return s[max(0, math.ceil(0.95 * len(s)) - 1)]
def cls(n):
    n = int(n); return "small" if n <= 16384 else ("mid" if n <= 1048576 else "large")
def regret(r, a):
    m, rr = float(r["med_M_ns"]), float(r["med_R_ns"])
    return (m if a == "M" else rr) / min(m, rr)
out = ["# POST HOC static baselines that include N (not preregistered)\n"]
for name, key in (("S4: per (dtype,k,N)", lambda r: (r["dtype"], r["k"], r["n"])),
                  ("S5: per (dtype,k,N-class small<=16Ki / mid 64Ki-1Mi / large>=4Mi)", lambda r: (r["dtype"], r["k"], cls(r["n"])))):
    groups = collections.defaultdict(list)
    for r in rows: groups[key(r)].append(r)
    choice = {}
    for g, rs in groups.items():
        lm = {a: st.fmean(math.log(regret(r, a)) for r in rs) for a in ("M", "R")}
        choice[g] = "M" if lm["M"] <= lm["R"] else "R"
    rg = [regret(r, choice[key(r)]) for r in rows]
    out.append(f"- {name}: cells {len(rg)}, median {st.median(rg):.3f}, p95 {p95(rg):.3f}, max {max(rg):.3f}, share>1.05 {sum(x>1.05 for x in rg)/len(rg):.3f}")
    for regime in ("arena_warm", "arena_cold", "glibc_default_fresh"):
        v = [regret(r, choice[key(r)]) for r in rows if r["regime"] == regime]
        out.append(f"  - {regime}: p95 {p95(v):.3f}, max {max(v):.3f}, share>1.05 {sum(x>1.05 for x in v)/len(v):.3f}")
open(os.path.join(d, "derived", "posthoc_s4.md"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
