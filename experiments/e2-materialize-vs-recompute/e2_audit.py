#!/usr/bin/env python3
"""E2 post hoc audit (NOT preregistered; every number is labelled POST HOC).

Re-derives, from the committed raw E2 data only, (1) the preregistered numbers
(validation: must equal analyze.py), (2) the same numbers with the N=64, k in {6,8}
accounting-collision cells excluded, (3) conservative gate-C variants C_STRICT and
C_ADJACENT, (4) S4 = best fixed action per (dtype,k,N) constant across regimes,
(5) S4-CV = the same chosen on two regimes and tested on the held-out third.

usage: e2_audit.py <results-dir> <label> <out.md>
"""
import collections
import csv
import glob
import math
import os
import random
import statistics as st
import sys

REGIMES = ["arena_warm", "arena_cold", "glibc_default_fresh"]
BOOT, SEED = 1000, 12345


def p95(v):
    s = sorted(v)
    return s[max(0, math.ceil(0.95 * len(s)) - 1)]


def load(d):
    S = collections.defaultdict(lambda: collections.defaultdict(dict))
    for f in sorted(glob.glob(os.path.join(d, "raw_*.csv"))):
        for r in csv.DictReader(open(f, newline="")):
            c = (r["dtype"], int(r["fanout"]), int(r["n"]), r["regime"])
            S[c][r["strategy"]][(r["replicate"], int(r["rep"]))] = float(r["ns_per_call"])
    return S


class Audit:
    def __init__(self, S):
        self.S = S
        self.all_cells = sorted(S, key=lambda c: (c[0], c[1], c[2], REGIMES.index(c[3])))
        self.reps = sorted({u[0] for c in S for u in S[c]["M"]})
        self.sizes = sorted({c[2] for c in S})
        self.stat = {c: {"M": st.median(S[c]["M"].values()), "R": st.median(S[c]["R"].values())} for c in S}

    def regret(self, c, a):
        s = self.stat[c]
        return s[a] / min(s["M"], s["R"])

    def decisive(self, c, units=None):
        S = self.S
        us = sorted(units if units is not None else set(S[c]["M"]) & set(S[c]["R"]))
        m, r = st.median(S[c]["M"][u] for u in us), st.median(S[c]["R"][u] for u in us)
        wm = sum(S[c]["M"][u] < S[c]["R"][u] for u in us) / len(us)
        wr = sum(S[c]["R"][u] < S[c]["M"][u] for u in us) / len(us)
        if m <= 0.97 * r and wm >= 0.75:
            return "M"
        if r <= 0.97 * m and wr >= 0.75:
            return "R"
        return None

    def stable(self, c1, w1, c2, w2):
        for rep in self.reps:
            u1 = [u for u in self.S[c1]["M"] if u[0] == rep]
            u2 = [u for u in self.S[c2]["M"] if u[0] == rep]
            if self.decisive(c1, u1) != w1 or self.decisive(c2, u2) != w2:
                return False
        return True

    def corroborated(self, c, a, rng):
        b = "R" if a == "M" else "M"
        if self.stat[c][a] / self.stat[c][b] <= 1.05:
            return False
        us = sorted(set(self.S[c][a]) & set(self.S[c][b]))
        per = [self.S[c][a][u] / self.S[c][b][u] for u in us]
        bs = sorted(st.median(per[rng.randrange(len(per))] for _ in per) for _ in range(BOOT))
        return bs[int(0.025 * BOOT)] > 1.0

    @staticmethod
    def summ(rg):
        return {"n": len(rg), "median": st.median(rg), "p95": p95(rg), "max": max(rg), "gt105": sum(x > 1.05 for x in rg) / len(rg)}

    def best_action(self, cs):
        lm = {a: st.fmean(math.log(self.regret(c, a)) for c in cs) for a in ("M", "R")}
        return "M" if lm["M"] <= lm["R"] else "R"  # ties: M

    def run(self, cells):
        A = self
        out = {"cells": len(cells)}
        by = collections.defaultdict(list)
        for c in cells:
            by[(c[0], c[1])].append(c)
        s3 = {g: A.best_action(cs) for g, cs in by.items()}
        rg = [A.regret(c, s3[(c[0], c[1])]) for c in cells]
        out["S3"] = A.summ(rg)
        rng = random.Random(SEED)
        opp = [c for c in cells if A.corroborated(c, s3[(c[0], c[1])], rng)]
        out["S3_opp"] = len(opp)
        # original gate C (either-order cost >= 5%), pooled decisive, stable in every replicate
        dec = {c: A.decisive(c) for c in cells}
        orig, strict = [], []
        for g, cs in by.items():
            for i, c1 in enumerate(cs):
                for c2 in cs[i + 1:]:
                    w1, w2 = dec[c1], dec[c2]
                    if w1 is None or w2 is None or w1 == w2:
                        continue
                    cost12 = A.stat[c2][w1] / A.stat[c2][w2]
                    cost21 = A.stat[c1][w2] / A.stat[c1][w1]
                    if max(cost12, cost21) < 1.05:
                        continue
                    if not A.stable(c1, w1, c2, w2):
                        continue
                    orig.append((c1, c2))
                    if min(cost12, cost21) >= 1.05:
                        strict.append((c1, c2))

        def kind(c1, c2):
            return "regime-only" if c1[2] == c2[2] else ("N-only" if c1[3] == c2[3] else "mixed")

        def adjacent(c1, c2):
            if c1[2] == c2[2] and c1[3] != c2[3]:
                return True
            if c1[3] == c2[3]:
                return abs(A.sizes.index(c1[2]) - A.sizes.index(c2[2])) == 1
            return False

        adj = [p for p in strict if adjacent(*p)]
        out["orig"] = {"pairs": len(orig), "groups": len({(p[0][0], p[0][1]) for p in orig}),
                       "kinds": collections.Counter(kind(*p) for p in orig)}
        out["strict"] = {"pairs": len(strict), "groups": len({(p[0][0], p[0][1]) for p in strict}),
                         "kinds": collections.Counter(kind(*p) for p in strict)}

        def semantic(p):
            c1, c2 = p
            if c1[3] == c2[3]:
                lo = min(c1[2], c2[2])
                return (c1[0], c1[1], c1[3], "N-boundary", lo)
            return (c1[0], c1[1], c1[2], "regime", tuple(sorted((c1[3], c2[3]))))

        out["adj"] = {"pairs": len(adj), "groups": len({(p[0][0], p[0][1]) for p in adj}),
                      "semantic": len({semantic(p) for p in adj}), "kinds": collections.Counter(kind(*p) for p in adj),
                      "list": sorted({semantic(p) for p in adj}, key=str)}
        # S4 and S4-CV
        g3 = collections.defaultdict(list)
        for c in cells:
            g3[(c[0], c[1], c[2])].append(c)
        s4 = {g: A.best_action(cs) for g, cs in g3.items()}
        out["S4"] = A.summ([A.regret(c, s4[(c[0], c[1], c[2])]) for c in cells])
        cv, cv_by_reg, cv_cells = [], collections.defaultdict(list), []
        for g, cs in g3.items():
            if len(cs) < 3:
                continue
            for h in cs:
                act = A.best_action([c for c in cs if c is not h])
                r = A.regret(h, act)
                cv.append(r)
                cv_by_reg[h[3]].append(r)
                cv_cells.append((h, act))
        out["S4cv"] = A.summ(cv)
        out["S4cv_reg"] = {k: A.summ(v) for k, v in cv_by_reg.items()}
        rng2 = random.Random(SEED)
        cvopp = [(h, a) for h, a in cv_cells if A.corroborated(h, a, rng2)]
        out["S4cv_opp"] = len(cvopp)
        out["S4cv_opp_cells"] = [(h, a, A.stat[h]["M"], A.stat[h]["R"], A.regret(h, a)) for h, a in cvopp]
        out["S4_opp"] = sum(A.corroborated(c, s4[(c[0], c[1], c[2])], random.Random(SEED)) for c in cells)
        out["S4_by_reg"] = {r: A.summ([A.regret(c, s4[(c[0], c[1], c[2])]) for c in cells if c[3] == r]) for r in REGIMES}
        return out


def fmt(o):
    L = []
    s3, s4, cv = o["S3"], o["S4"], o["S4cv"]
    L.append(f"| cells | {o['cells']} |")
    L.append(f"| S3 median / p95 / max / >1.05 | {s3['median']:.3f} / {s3['p95']:.3f} / {s3['max']:.3f} / {s3['gt105']:.3f} |")
    L.append(f"| S3 corroborated opportunity cells (rate) | {o['S3_opp']} ({o['S3_opp'] / o['cells']:.3f}) |")
    L.append(f"| original Gate C: stable pairs / unique (dtype,k) groups | {o['orig']['pairs']} / {o['orig']['groups']} |")
    L.append(f"| original Gate C by kind | {dict(o['orig']['kinds'])} |")
    L.append(f"| C_STRICT: pairs / (dtype,k) groups | {o['strict']['pairs']} / {o['strict']['groups']} |")
    L.append(f"| C_STRICT by kind | {dict(o['strict']['kinds'])} |")
    L.append(f"| C_ADJACENT: pairs / (dtype,k) groups / semantic groups | {o['adj']['pairs']} / {o['adj']['groups']} / {o['adj']['semantic']} |")
    L.append(f"| C_ADJACENT by kind | {dict(o['adj']['kinds'])} |")
    L.append(f"| S4 median / p95 / max / >1.05 | {s4['median']:.3f} / {s4['p95']:.3f} / {s4['max']:.3f} / {s4['gt105']:.3f} |")
    L.append(f"| S4 corroborated opportunity cells | {o['S4_opp']} |")
    L.append(f"| S4 p95 / max by regime | " + "; ".join(f"{r}: {v['p95']:.3f} / {v['max']:.3f}" for r, v in o["S4_by_reg"].items()) + " |")
    L.append(f"| S4-CV median / p95 / max / >1.05 | {cv['median']:.3f} / {cv['p95']:.3f} / {cv['max']:.3f} / {cv['gt105']:.3f} |")
    L.append(f"| S4-CV corroborated opportunity cells (rate) | {o['S4cv_opp']} ({o['S4cv_opp'] / cv['n']:.3f}) |")
    L.append(f"| S4-CV p95 / max by held-out regime | " + "; ".join(f"{r}: {v['p95']:.3f} / {v['max']:.3f} / >1.05 {v['gt105']:.3f}" for r, v in o["S4cv_reg"].items()) + " |")
    return L


def main():
    d, label, outp = sys.argv[1:4]
    A = Audit(load(d))
    full = A.run(A.all_cells)
    coll = {c for c in A.all_cells if c[2] == 64 and c[1] in (6, 8)}
    excl = A.run([c for c in A.all_cells if c not in coll])
    L = [f"## {label} (POST HOC audit of committed raw data)\n",
         f"Excluded accounting-collision cells: {len(coll)} (N = 64, k in {{6, 8}}, both dtypes, all three regimes).\n",
         "### All 324 cells (must equal the preregistered analysis)\n", "| quantity | value |", "| --- | --- |"] + fmt(full) + \
        ["", "### Excluding the collision cells\n", "| quantity | value |", "| --- | --- |"] + fmt(excl)
    L += ["", "C_ADJACENT semantic groups (all cells), `(dtype, k, regime, 'N-boundary', lower N)` or `(dtype, k, N, 'regime', pair)`:\n"]
    L += [f"- {x}" for x in full["adj"]["list"]]
    L += ["", "S4-CV corroborated opportunity cells (all 324 cells; chosen without the cell's own regime):", "",
          "| dtype | k | N | held-out regime | CV action | M ns | R ns | regret |", "| --- | ---: | ---: | --- | --- | ---: | ---: | ---: |"]
    L += [f"| {h[0]} | {h[1]} | {h[2]} | {h[3]} | {a} | {m:.0f} | {r:.0f} | {g:.3f} |" for h, a, m, r, g in full["S4cv_opp_cells"]]
    open(outp, "w").write("\n".join(L) + "\n")
    print("\n".join(L[:20]))


if __name__ == "__main__":
    main()
