#!/usr/bin/env python3
"""E2 analysis (pure standard library). Implements section 9-12 of
docs/experiments/e2-materialize-vs-recompute.md exactly; it decides nothing
beyond what the preregistration defines.

usage: analyze.py <results-dir> [--out derived]
reads raw_*.csv and strategies_*.csv in <results-dir>, writes
<results-dir>/derived/{cells.csv,e2_tables.md,summary.json}.
"""
import collections
import csv
import glob
import json
import math
import os
import random
import statistics as st
import sys

KS = [1, 2, 3, 4, 6, 8]
TS = [0, 1, 2, 3, 4, 6, 8]            # S2: R iff k <= T
REGIMES = ["arena_warm", "arena_cold", "glibc_default_fresh"]
BOOT = 1000
BOOT_SEED = 12345


def p95(values):
    s = sorted(values)
    return s[max(0, math.ceil(0.95 * len(s)) - 1)]


def load(d):
    raw = []
    for f in sorted(glob.glob(os.path.join(d, "raw_*.csv"))):
        raw += list(csv.DictReader(open(f, newline="")))
    plans = []
    for f in sorted(glob.glob(os.path.join(d, "strategies_*.csv"))):
        plans += list(csv.DictReader(open(f, newline="")))
    return raw, plans


def main():
    d = sys.argv[1]
    raw, plans = load(d)
    if not raw:
        sys.exit("no raw data")
    # unit = (replicate, rep): all series of one repetition are adjacent in time.
    samples = collections.defaultdict(lambda: collections.defaultdict(dict))  # cell -> strategy -> unit -> ns
    bad = 0
    rows = collections.defaultdict(list)
    for r in raw:
        cell = (r["dtype"], int(r["fanout"]), int(r["n"]), r["regime"])
        unit = (r["replicate"], int(r["rep"]))
        samples[cell][r["strategy"]][unit] = float(r["ns_per_call"])
        rows[cell].append(r)
        if r["correct"] != "1":
            bad += 1
    cells = sorted(samples, key=lambda c: (c[0], c[1], c[2], REGIMES.index(c[3]) if c[3] in REGIMES else 9))
    dtypes = sorted({c[0] for c in cells})
    sizes = sorted({c[2] for c in cells})
    regimes = [r for r in REGIMES if any(c[3] == r for c in cells)]
    ks = sorted({c[1] for c in cells})
    replicates = sorted({u[0] for c in cells for u in samples[c]["M"]})

    def med(cell, s, units=None):
        v = samples[cell][s]
        return st.median(v[u] for u in (units if units is not None else v))

    stats = {}
    for c in cells:
        mM, mR, mT = med(c, "M"), med(c, "R"), med(c, "M_TWIN")
        best = min(mM, mR)
        stats[c] = {"M": mM, "R": mR, "TW": mT, "best": best, "win": "M" if mM <= mR else "R"}

    def regret(c, choice):
        return stats[c][choice] / stats[c]["best"]

    # ---- static baselines
    def byte_model(k, tie):
        return "R" if k <= 2 else ("M" if k >= 4 else tie)

    def mean_log(cs, choice_of):
        return st.fmean(math.log(regret(c, choice_of(c))) for c in cs)

    s1 = {t: {c: byte_model(c[1], t) for c in cells} for t in ("R", "M")}
    s2_all = {T: mean_log(cells, lambda c, T=T: "R" if c[1] <= T else "M") for T in TS}
    best_T = min(TS, key=lambda T: (s2_all[T], T))
    s2_dt = {}
    for dt in dtypes:
        cs = [c for c in cells if c[0] == dt]
        sc = {T: mean_log(cs, lambda c, T=T: "R" if c[1] <= T else "M") for T in TS}
        s2_dt[dt] = min(TS, key=lambda T: (sc[T], T))
    s3 = {}
    for dt in dtypes:
        for k in ks:
            cs = [c for c in cells if c[0] == dt and c[1] == k]
            lm = {a: mean_log(cs, lambda c, a=a: a) for a in ("M", "R")}
            s3[(dt, k)] = "M" if lm["M"] <= lm["R"] else "R"  # ties: M

    policies = {
        "S1a (byte rule, k=3->R)": lambda c: s1["R"][c],
        "S1b (byte rule, k=3->M)": lambda c: s1["M"][c],
        f"S2 (best global T={best_T})": lambda c: "R" if c[1] <= best_T else "M",
        "S3 (best static lookup per dtype,k)": lambda c: s3[(c[0], c[1])],
        "ALWAYS_M": lambda c: "M",
        "ALWAYS_R": lambda c: "R",
    }

    def summarize(cs, pol):
        rg = [regret(c, pol(c)) for c in cs]
        return {"n": len(rg), "median": st.median(rg), "p95": p95(rg), "max": max(rg),
                "gt105": sum(1 for x in rg if x > 1.05) / len(rg), "gt115": sum(1 for x in rg if x > 1.15) / len(rg)}

    # ---- paired bootstrap for corroborated opportunity (gate A)
    rng = random.Random(BOOT_SEED)

    def corroborated(c, a, b):
        ratio_med = stats[c][a] / stats[c][b]
        if ratio_med <= 1.05:
            return False
        units = sorted(set(samples[c][a]) & set(samples[c][b]))
        per = [samples[c][a][u] / samples[c][b][u] for u in units]
        boots = []
        for _ in range(BOOT):
            boots.append(st.median(per[rng.randrange(len(per))] for _ in per))
        boots.sort()
        return boots[int(0.025 * BOOT)] > 1.0

    def opportunity_cells(cs, pol):
        out = []
        for c in cs:
            a = pol(c)
            b = "R" if a == "M" else "M"
            if corroborated(c, a, b):
                out.append(c)
        return out

    def decisive(c, units=None):
        """Winner median >= 3% lower and faster in >= 75% of the units; else None."""
        us = sorted(units if units is not None else set(samples[c]["M"]) & set(samples[c]["R"]))
        m, r = st.median(samples[c]["M"][u] for u in us), st.median(samples[c]["R"][u] for u in us)
        wins_m = sum(1 for u in us if samples[c]["M"][u] < samples[c]["R"][u]) / len(us)
        wins_r = sum(1 for u in us if samples[c]["R"][u] < samples[c]["M"][u]) / len(us)
        if m <= 0.97 * r and wins_m >= 0.75:
            return "M"
        if r <= 0.97 * m and wins_r >= 0.75:
            return "R"
        return None

    def flips(cs_filter):
        """Gate C. For each (dtype,k), unordered cell pairs with opposite decisive winners where the
        first's winner costs >= 5% in the second (either order), reproduced in every replicate alone."""
        found = []
        for dt in dtypes:
            for k in ks:
                cs = [c for c in cs_filter if c[0] == dt and c[1] == k]
                dec = {c: decisive(c) for c in cs}
                for i, c1 in enumerate(cs):
                    for c2 in cs[i + 1:]:
                        w1, w2 = dec[c1], dec[c2]
                        if w1 is None or w2 is None or w1 == w2:
                            continue
                        cost12 = stats[c2][w1] / stats[c2][w2]  # w1 run where w2 wins
                        cost21 = stats[c1][w2] / stats[c1][w1]
                        if max(cost12, cost21) < 1.05:
                            continue
                        stable = True
                        for rep in replicates:
                            u1 = [u for u in set(samples[c1]["M"]) if u[0] == rep]
                            u2 = [u for u in set(samples[c2]["M"]) if u[0] == rep]
                            if decisive(c1, u1) != w1 or decisive(c2, u2) != w2:
                                stable = False
                                break
                        found.append({"dtype": dt, "k": k, "cell1": c1, "w1": w1, "cell2": c2, "w2": w2,
                                      "cost": max(cost12, cost21), "stable": stable})
        return found

    # ---- gates
    def gates(cs, label):
        pol = policies["S3 (best static lookup per dtype,k)"]
        sm = summarize(cs, pol)
        opp = opportunity_cells(cs, pol)
        fl = [f for f in flips(cs) if f["stable"]]
        A = len(opp) / len(cs) >= 0.20
        B = sm["p95"] >= 1.15
        C = len(fl) > 0
        kill = sm["gt105"] <= 0.05 and sm["p95"] < 1.15 and not C
        return {"label": label, "cells": len(cs), "S3_median": sm["median"], "S3_p95": sm["p95"], "S3_max": sm["max"],
                "S3_gt105": sm["gt105"], "opportunity_cells": len(opp), "opportunity_rate": len(opp) / len(cs),
                "gate_A": A, "gate_B": B, "gate_C": C, "stable_flips": len(fl), "survive": A or B or C, "kill_condition": kill,
                "_opp": opp, "_flips": fl}

    G = gates(cells, "all cells")
    G_nok1 = gates([c for c in cells if c[1] != 1], "excluding k=1 (control)")
    per_regime = {rg: gates([c for c in cells if c[3] == rg], rg) for rg in regimes}

    # ---- noise floor (twin = a second series of M)
    twin = [max(stats[c]["M"], stats[c]["TW"]) / min(stats[c]["M"], stats[c]["TW"]) for c in cells]
    twin_by_regime = {rg: [max(stats[c]["M"], stats[c]["TW"]) / min(stats[c]["M"], stats[c]["TW"]) for c in cells if c[3] == rg]
                      for rg in regimes}

    # ---- k* map
    def kstar(dt, n, rg):
        dec = {k: decisive((dt, k, n, rg)) for k in ks if (dt, k, n, rg) in samples}
        seq = [dec[k] for k in sorted(dec)]
        if all(x == "M" for x in seq):
            return "all"
        for i, k in enumerate(sorted(dec)):
            if all(x == "M" for x in seq[i:]):
                return str(k)
        return "none"

    # ---- byte model prediction vs empirical winner
    model_rows = []
    for c in cells:
        pred = byte_model(c[1], "-")
        model_rows.append((c, pred, stats[c]["win"], decisive(c)))

    # ---- correctness / fairness / environment bookkeeping
    nsamples = len(raw)
    fair = collections.defaultdict(dict)
    for p in plans:
        if p["strategy"] in ("M", "R"):
            fair[(p["dtype"], int(p["fanout"]), int(p["n"]))].setdefault(p["strategy"], set()).add(
                (p["fresh"], p["copies"], p["moves"], p["dry_result_allocs"], p["dry_peak_live_bytes"]))
    unfair = [k for k, v in fair.items() if v.get("M") != v.get("R")]
    fallbacks = sum(int(r["arena_fallbacks"]) for r in raw)
    overflows = sum(int(r["table_overflows"]) for r in raw)
    warm_faults = [float(r["minflt_per_call"]) for r in raw if r["regime"] == "arena_warm"]

    # ================= output
    outd = os.path.join(d, "derived")
    os.makedirs(outd, exist_ok=True)
    with open(os.path.join(outd, "cells.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["dtype", "k", "n", "regime", "med_M_ns", "med_R_ns", "med_TWIN_ns", "M_over_R", "winner", "decisive",
                    "byte_model", "S3_choice", "S3_regret", "S1a_regret", "S1b_regret", "S2_regret", "units"])
        for c in cells:
            w.writerow([c[0], c[1], c[2], c[3], f"{stats[c]['M']:.1f}", f"{stats[c]['R']:.1f}", f"{stats[c]['TW']:.1f}",
                        f"{stats[c]['M'] / stats[c]['R']:.4f}", stats[c]["win"], decisive(c) or "", byte_model(c[1], "tie"),
                        s3[(c[0], c[1])], f"{regret(c, s3[(c[0], c[1])]):.4f}", f"{regret(c, s1['R'][c]):.4f}",
                        f"{regret(c, s1['M'][c]):.4f}", f"{regret(c, 'R' if c[1] <= best_T else 'M'):.4f}",
                        len(set(samples[c]['M']) & set(samples[c]['R']))])

    L = ["# E2 tables (generated by analyze.py)\n"]
    L.append(f"Cells: {len(cells)}; raw timed samples: {nsamples}; replicates: {len(replicates)}; "
             f"units per cell: {len(set(samples[cells[0]]['M']))}.\n")
    L.append("## Integrity\n")
    L.append(f"- samples with correct != 1: {bad}\n- arena fallbacks: {fallbacks}; hook table overflows: {overflows}")
    L.append(f"- arena_warm minor faults per call: max {max(warm_faults) if warm_faults else float('nan'):.4f}, "
             f"median {st.median(warm_faults) if warm_faults else float('nan'):.4f}")
    L.append(f"- cells where M and R differ in fresh allocations, copies, moves or result allocs: {len(unfair)}\n")
    L.append("## Baselines: regret over all cells (regret = median(choice)/median(best))\n")
    L.append("| policy | cells | median | p95 | max | share > 1.05 | share > 1.15 |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for name, pol in policies.items():
        s = summarize(cells, pol)
        L.append(f"| {name} | {s['n']} | {s['median']:.3f} | {s['p95']:.3f} | {s['max']:.3f} | {s['gt105']:.3f} | {s['gt115']:.3f} |")
    L.append("")
    L.append(f"Best global threshold T (S2, R iff k <= T): **{best_T}**; per dtype: {s2_dt}. Mean log-regret by T: "
             + ", ".join(f"T={T}: {s2_all[T]:.4f}" for T in TS) + "\n")
    L.append("S3 lookup (dtype, k) -> action: " + ", ".join(f"{dt}/k{k}:{s3[(dt, k)]}" for dt in dtypes for k in ks) + "\n")
    L.append("### S3 by regime\n")
    L.append("| regime | cells | median | p95 | max | share > 1.05 |\n| --- | ---: | ---: | ---: | ---: | ---: |")
    pol3 = policies["S3 (best static lookup per dtype,k)"]
    for rg in regimes:
        s = summarize([c for c in cells if c[3] == rg], pol3)
        L.append(f"| {rg} | {s['n']} | {s['median']:.3f} | {s['p95']:.3f} | {s['max']:.3f} | {s['gt105']:.3f} |")
    L.append("")
    L.append("## Gates (primary baseline S3)\n")
    L.append("| set | cells | S3 median | S3 p95 | S3 max | S3 share > 1.05 | corroborated opportunity cells | rate | A (>=20%) | B (p95>=1.15) | C (stable flip) | stable flips | survive | kill condition |")
    L.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | --- | ---: | --- | --- |")
    for g in [G, G_nok1] + list(per_regime.values()):
        L.append(f"| {g['label']} | {g['cells']} | {g['S3_median']:.3f} | {g['S3_p95']:.3f} | {g['S3_max']:.3f} | {g['S3_gt105']:.3f} | "
                 f"{g['opportunity_cells']} | {g['opportunity_rate']:.3f} | {g['gate_A']} | {g['gate_B']} | {g['gate_C']} | {g['stable_flips']} | "
                 f"{g['survive']} | {g['kill_condition']} |")
    L.append("")
    L.append("### Corroborated opportunity cells (S3)\n")
    L.append("| dtype | k | n | regime | S3 choice | M ns | R ns | S3 regret |\n| --- | ---: | ---: | --- | --- | ---: | ---: | ---: |")
    for c in G["_opp"]:
        L.append(f"| {c[0]} | {c[1]} | {c[2]} | {c[3]} | {s3[(c[0], c[1])]} | {stats[c]['M']:.0f} | {stats[c]['R']:.0f} | {regret(c, s3[(c[0], c[1])]):.3f} |")
    if not G["_opp"]:
        L.append("| (none) | | | | | | | |")
    L.append("")
    L.append("### Stable same-(dtype,k) flips (all three replicates alone)\n")
    L.append("| dtype | k | cell 1 (n, regime) | winner 1 | cell 2 (n, regime) | winner 2 | cost of wrong static action |\n| --- | ---: | --- | --- | --- | --- | ---: |")
    for f in G["_flips"]:
        L.append(f"| {f['dtype']} | {f['k']} | {f['cell1'][2]}, {f['cell1'][3]} | {f['w1']} | {f['cell2'][2]}, {f['cell2'][3]} | {f['w2']} | {f['cost']:.3f} |")
    if not G["_flips"]:
        L.append("| (none) | | | | | | |")
    unstable = [f for f in flips(cells) if not f["stable"]]
    L.append(f"\nPairs with opposite decisive winners and cost >= 5% in the pooled data that did NOT reproduce in every replicate: {len(unstable)}\n")
    L.append("## Noise floor (M versus its own twin series, ratio of larger to smaller median)\n")
    L.append("| set | cells | median | p95 | max | share > 1.05 |\n| --- | ---: | ---: | ---: | ---: | ---: |")
    for name, v in [("all", twin)] + [(rg, twin_by_regime[rg]) for rg in regimes]:
        L.append(f"| {name} | {len(v)} | {st.median(v):.3f} | {p95(v):.3f} | {max(v):.3f} | {sum(1 for x in v if x > 1.05) / len(v):.3f} |")
    L.append("")
    L.append("## Crossover k* (smallest tested k from which M wins decisively at every larger tested k)\n")
    L.append("`all` = M decisive from k=1; `none` = M not decisive at the largest tested k or not a suffix.\n")
    L.append("| dtype | N | " + " | ".join(regimes) + " |\n| --- | ---: | " + " | ".join("---" for _ in regimes) + " |")
    kstar_map = {}
    for dt in dtypes:
        for n in sizes:
            vals = [kstar(dt, n, rg) for rg in regimes]
            kstar_map[f"{dt}/{n}"] = dict(zip(regimes, vals))
            L.append(f"| {dt} | {n} | " + " | ".join(vals) + " |")
    L.append("")
    L.append("## Byte model prediction versus empirical winner (k = 3 excluded: a tie in the model)\n")
    L.append("| regime | cells (k != 3) | agree (median winner) | agree where decisive | decisive cells (k != 3) |\n| --- | ---: | ---: | ---: | ---: |")
    for rg in regimes:
        rs = [(c, p, w, dcs) for c, p, w, dcs in model_rows if c[3] == rg and c[1] != 3]
        ag = sum(1 for _, p, w, _ in rs if p == w)
        dcs_rows = [x for x in rs if x[3] is not None]
        agd = sum(1 for _, p, _, dcs in dcs_rows if p == dcs)
        L.append(f"| {rg} | {len(rs)} | {ag} ({ag / len(rs):.2f}) | {agd}/{len(dcs_rows)} | {len(dcs_rows)} |")
    L.append("")
    L.append("### M/R ratio (median M ns / median R ns; > 1 means R is faster), by dtype, k, regime and N\n")
    for dt in dtypes:
        L.append(f"**{dt}**\n")
        for rg in regimes:
            L.append(f"{rg}\n")
            L.append("| k \\ N | " + " | ".join(str(n) for n in sizes) + " |\n| --- | " + " | ".join("---:" for _ in sizes) + " |")
            for k in ks:
                cs = [stats.get((dt, k, n, rg)) for n in sizes]
                L.append(f"| {k} | " + " | ".join(f"{x['M'] / x['R']:.3f}" if x else "" for x in cs) + " |")
            L.append("")
    with open(os.path.join(outd, "e2_tables.md"), "w") as fh:
        fh.write("\n".join(L) + "\n")
    summ = {"cells": len(cells), "samples": nsamples, "best_T": best_T, "s2_per_dtype": s2_dt,
            "s3": {f"{k[0]}/{k[1]}": v for k, v in s3.items()},
            "gates": {g["label"]: {k: v for k, v in g.items() if not k.startswith("_")} for g in [G, G_nok1] + list(per_regime.values())},
            "policies": {n: summarize(cells, p) for n, p in policies.items()},
            "twin": {"median": st.median(twin), "p95": p95(twin), "max": max(twin)},
            "kstar": kstar_map, "incorrect_samples": bad}
    with open(os.path.join(outd, "summary.json"), "w") as fh:
        json.dump(summ, fh, indent=1, default=str)
    print(f"cells={len(cells)} samples={nsamples} bestT={best_T} S3 p95={G['S3_p95']:.3f} survive={G['survive']}")


if __name__ == "__main__":
    main()
