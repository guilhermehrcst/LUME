#!/usr/bin/env python3
"""Post-hoc robustness view: fixed-policy regret computed separately for each
replicate (15 repetitions each) instead of pooled. Descriptive; the gates use the
pooled analysis in analyze.py.
usage: per_replicate_regret.py <results-dir>"""
import collections
import csv
import glob
import math
import os
import statistics as st
import sys

d = sys.argv[1]
raw, plans = [], []
for f in sorted(glob.glob(os.path.join(d, "raw_*.csv"))):
    raw += list(csv.DictReader(open(f, newline="")))
for f in sorted(glob.glob(os.path.join(d, "plans_*.csv"))):
    plans += list(csv.DictReader(open(f, newline="")))
info = collections.defaultdict(dict)
for r in plans:
    info[(r["program"], int(r["n"]))].setdefault(r["plan"], r)
t = collections.defaultdict(list)
for r in raw:
    t[(r["replicate"], r["program"], int(r["n"]), r["regime"], r["plan"])].append(float(r["ns_per_call"]))
cells = sorted({k[:4] for k in t})
print("replicate | decision cells | median | p95 (nearest rank) | max | cells > 1.05")
for rep in sorted({c[0] for c in cells}):
    regs = []
    for (r, program, n, regime) in cells:
        if r != rep:
            continue
        strat = [l for l, row in info[(program, n)].items() if row["is_twin"] == "0"]
        if len(strat) < 2:
            continue
        fixed = next(l for l, row in info[(program, n)].items() if row["is_fixed"] == "1")
        med = {l: st.median(t[(r, program, n, regime, l)]) for l in strat}
        regs.append(med[fixed] / min(med.values()))
    srt = sorted(regs)
    p95 = srt[max(0, math.ceil(0.95 * len(srt)) - 1)]
    print(f"{rep} | {len(regs)} | {st.median(regs):.4f} | {p95:.4f} | {max(regs):.4f} | {sum(1 for x in regs if x > 1.05)}")
