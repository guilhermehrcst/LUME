#!/usr/bin/env python3
"""E1 analysis. Pure standard library. Implements the preregistered rules in
docs/experiments/e1-is-planning-necessary.md (Part I, sections 9 and 10) and
nothing else; it contains no tunable that was chosen after seeing data.

usage: analyze.py <results-dir> [--out <derived-dir>] [--with-supplementary]
  By default only the preregistered regimes are analysed (the gates of the
  preregistration). --with-supplementary also includes post-hoc regimes
  (glibc_default_fresh); that run is reported separately and never replaces
  the preregistered one.
  <results-dir>/raw_*.csv    one row per timed sample (written by lume_e1_bench)
  <results-dir>/plans_*.csv  one row per distinct plan per program x n x replicate
Writes cells.csv, gates.json, summary.md and flips.csv into the derived dir.
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

REGIMES = ["glibc_default", "glibc_t1", "glibc_t2", "arena_cold", "arena_warm"]   # preregistered
SUPPLEMENTARY_REGIMES = ["glibc_default_fresh"]                                      # post hoc, labelled
BOOT = 1000
BOOT_SEED = 20260505
OPP_REGRET = 1.05          # preregistered: regret above 5 %
TAIL_P95 = 1.15            # preregistered: p95 regret gate
OPP_RATE = 0.20            # preregistered: share of decision cells
DECISIVE_RATIO = 0.97      # preregistered: winner median <= 0.97 x every other
DECISIVE_UNITS = 0.75      # preregistered: and faster in >= 75 % of units
FLIP_COST = 1.05           # preregistered: wrong plan costs >= 5 %
KILL_WITHIN = 0.95         # preregistered: >= 95 % of cells within 5 %


def median(xs):
    return st.median(xs)


def pctl_nearest_rank(xs, p):
    s = sorted(xs)
    return s[max(0, math.ceil(p * len(s)) - 1)]


def pctl_linear(xs, p):
    s = sorted(xs)
    if len(s) == 1:
        return s[0]
    pos = p * (len(s) - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def load(results_dir):
    raw, plans = [], []
    for f in sorted(glob.glob(os.path.join(results_dir, "raw_*.csv"))):
        with open(f, newline="") as fh:
            raw.extend(csv.DictReader(fh))
    for f in sorted(glob.glob(os.path.join(results_dir, "plans_*.csv"))):
        with open(f, newline="") as fh:
            plans.extend(csv.DictReader(fh))
    if not raw or not plans:
        raise SystemExit("no data in " + results_dir)
    return raw, plans


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    results_dir = sys.argv[1]
    out_dir = os.path.join(results_dir, "derived")
    if "--out" in sys.argv:
        out_dir = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out_dir, exist_ok=True)
    raw, plans = load(results_dir)
    allowed = REGIMES + (SUPPLEMENTARY_REGIMES if "--with-supplementary" in sys.argv else [])
    raw = [r for r in raw if r["regime"] in allowed]
    if not raw:
        raise SystemExit("no samples in the selected regimes")

    # ------------------------------------------------------------ validity
    invalid = [r for r in raw if r["correct"] != "1" or int(r["arena_fallbacks"]) != 0 or int(r["table_overflows"]) != 0]
    if invalid:
        raise SystemExit(f"{len(invalid)} invalid sample rows (correctness / workspace / tracking); refusing to analyse")

    # ------------------------------------------------------------ structure
    # plan info: (program, n) -> {label: row} (first replicate; identical across replicates)
    plan_info = collections.defaultdict(dict)
    for r in plans:
        plan_info[(r["program"], int(r["n"]))].setdefault(r["plan"], r)
    # samples: cell -> label -> {(replicate, rep): ns}
    samples = collections.defaultdict(lambda: collections.defaultdict(dict))
    faults = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in raw:
        cell = (r["program"], int(r["n"]), r["regime"])
        samples[cell][r["plan"]][(r["replicate"], int(r["rep"]))] = float(r["ns_per_call"])
        faults[cell][r["plan"]].append(float(r["minflt_per_call"]))
    replicates = sorted({r["replicate"] for r in raw})

    def strategies(program, n):
        return [l for l, row in plan_info[(program, n)].items() if row["is_twin"] == "0"]

    def fixed_label(program, n):
        return next(l for l, row in plan_info[(program, n)].items() if row["is_fixed"] == "1")

    rng = random.Random(BOOT_SEED)
    cell_rows = []
    decision = []
    for cell in sorted(samples, key=lambda c: (c[0], c[1], (REGIMES + SUPPLEMENTARY_REGIMES).index(c[2]))):
        program, n, regime = cell
        strat = strategies(program, n)
        by = samples[cell]
        units = sorted(set.intersection(*[set(by[l]) for l in by]))
        med = {l: median([by[l][u] for u in units]) for l in by}
        fixed = fixed_label(program, n)
        best = min(strat, key=lambda l: med[l])
        ranking = sorted(strat, key=lambda l: med[l])
        second = ranking[1] if len(ranking) > 1 else ""
        twin = med.get("FIXED_TWIN")
        row = {
            "program": program, "n": n, "regime": regime, "units": len(units),
            "distinct_plans": len(strat), "plans": "|".join(plan_info[(program, n)][l]["group"] for l in strat),
            "empirical_best": best, "best_ns": f"{med[best]:.1f}", "fixed_plan": fixed, "fixed_ns": f"{med[fixed]:.1f}",
            "fixed_regret": f"{med[fixed] / med[best]:.4f}", "second_best": second,
            "second_best_regret": f"{med[second] / med[best]:.4f}" if second else "",
            "ranking": ">".join(ranking), "twin_regret": f"{med[fixed] / min(med[fixed], twin):.4f}" if twin else "",
            "opportunity": "", "opp_ratio": "", "opp_ci_low": "", "decisive_winner": "",
        }
        for l in strat:
            row[f"ns_{l}"] = f"{med[l]:.1f}"
        row["fixed_minflt"] = f"{median(faults[cell][fixed]):.2f}"
        row["best_minflt"] = f"{median(faults[cell][best]):.2f}"
        if len(strat) >= 2:
            decision.append(cell)
            # noise-corroborated opportunity (preregistered section 9)
            opp, best_ratio, best_lo, best_q = False, 0.0, 0.0, ""
            for q in strat:
                if q == fixed:
                    continue
                ratios = [by[fixed][u] / by[q][u] for u in units]
                point = median(ratios)
                boots = []
                for _ in range(BOOT):
                    sample = [ratios[rng.randrange(len(ratios))] for _ in ratios]
                    boots.append(median(sample))
                lo = pctl_linear(boots, 0.025)
                if point > OPP_REGRET and lo > 1.0:
                    opp = True
                if point > best_ratio:
                    best_ratio, best_lo, best_q = point, lo, q
            row["opportunity"] = int(opp)
            row["opp_ratio"] = f"{best_ratio:.4f}"
            row["opp_ci_low"] = f"{best_lo:.4f}"
            row["opp_vs"] = best_q
        cell_rows.append(row)

    # ------------------------------------------------------------ decisive winners
    def decisive_winner(by, units, strat):
        meds = {l: median([by[l][u] for u in units]) for l in strat}
        for w in strat:
            ok = True
            for o in strat:
                if o == w:
                    continue
                wins = sum(1 for u in units if by[w][u] < by[o][u]) / len(units)
                if not (meds[w] <= DECISIVE_RATIO * meds[o] and wins >= DECISIVE_UNITS):
                    ok = False
                    break
            if ok:
                return w
        return None

    winner = {}      # (program, n, regime) -> pooled decisive winner or None
    winner_rep = {}  # (program, n, regime, replicate) -> winner or None
    for cell in decision:
        program, n, regime = cell
        strat = strategies(program, n)
        by = samples[cell]
        units = sorted(set.intersection(*[set(by[l]) for l in strat]))
        winner[cell] = decisive_winner(by, units, strat)
        for rep_id in replicates:
            ru = [u for u in units if u[0] == rep_id]
            winner_rep[cell + (rep_id,)] = decisive_winner(by, ru, strat) if ru else None
    for row in cell_rows:
        c = (row["program"], row["n"], row["regime"])
        if c in winner:
            row["decisive_winner"] = winner[c] or ""

    def med_of(cell, label):
        by = samples[cell]
        units = sorted(set.intersection(*[set(by[l]) for l in strategies(cell[0], cell[1])]))
        return median([by[label][u] for u in units])

    # ------------------------------------------------------------ flips
    flips = []
    programs = sorted({c[0] for c in decision})
    for program in programs:
        cells = [c for c in decision if c[0] == program]
        for kind in ("regime", "size"):
            for c1 in cells:
                for c2 in cells:
                    if c1 >= c2:
                        continue
                    if kind == "regime" and not (c1[1] == c2[1] and c1[2] != c2[2]):
                        continue
                    if kind == "size" and not (c1[2] == c2[2] and c1[1] != c2[1]):
                        continue
                    w1, w2 = winner.get(c1), winner.get(c2)
                    if w1 is None or w2 is None or w1 == w2:
                        continue
                    # running w1 in c2 must cost >= 5 % relative to w2 there
                    cost = med_of(c2, w1) / med_of(c2, w2)
                    cost_rev = med_of(c1, w2) / med_of(c1, w1)
                    meaningful = cost >= FLIP_COST or cost_rev >= FLIP_COST
                    stable = all(winner_rep.get(c1 + (r,)) == w1 and winner_rep.get(c2 + (r,)) == w2 for r in replicates)
                    flips.append({"kind": kind, "program": program, "cell1": f"{c1[1]}/{c1[2]}", "winner1": w1,
                                  "cell2": f"{c2[1]}/{c2[2]}", "winner2": w2, "cost_w1_in_c2": f"{cost:.4f}",
                                  "cost_w2_in_c1": f"{cost_rev:.4f}", "meaningful": int(meaningful), "stable": int(stable)})
    stable_flips = [f for f in flips if f["meaningful"] and f["stable"]]

    # ------------------------------------------------------------ gates
    d_rows = [r for r in cell_rows if r["distinct_plans"] >= 2]
    regrets = [float(r["fixed_regret"]) for r in d_rows]
    twin_regrets = [float(r["twin_regret"]) for r in d_rows if r["twin_regret"]]
    n_dec = len(d_rows)
    raw_opp = sum(1 for x in regrets if x > OPP_REGRET)
    corr_opp = sum(1 for r in d_rows if r["opportunity"] == 1)
    within5 = sum(1 for x in regrets if x <= OPP_REGRET)
    gates = {
        "decision_cells": n_dec,
        "replicates": replicates,
        "raw_cells_regret_gt_1.05": raw_opp,
        "raw_opportunity_rate": raw_opp / n_dec,
        "corroborated_opportunity_cells": corr_opp,
        "corroborated_opportunity_rate": corr_opp / n_dec,
        "median_regret": median(regrets),
        "p95_regret_nearest_rank": pctl_nearest_rank(regrets, 0.95),
        "p95_regret_linear": pctl_linear(regrets, 0.95),
        "max_regret": max(regrets),
        "share_within_5pct": within5 / n_dec,
        "twin_null_median": median(twin_regrets) if twin_regrets else None,
        "twin_null_p95_nearest_rank": pctl_nearest_rank(twin_regrets, 0.95) if twin_regrets else None,
        "twin_null_max": max(twin_regrets) if twin_regrets else None,
        "twin_null_share_gt_1.05": (sum(1 for x in twin_regrets if x > OPP_REGRET) / len(twin_regrets)) if twin_regrets else None,
        "stable_meaningful_flips": len(stable_flips),
        "flips_all_decisive_pairs": len(flips),
    }
    gates["A_opportunity_rate_ge_20pct"] = gates["corroborated_opportunity_rate"] >= OPP_RATE
    gates["B_p95_regret_ge_1.15"] = gates["p95_regret_nearest_rank"] >= TAIL_P95
    gates["C_stable_flip"] = len(stable_flips) > 0
    gates["kill_condition_strictly_met"] = (gates["share_within_5pct"] >= KILL_WITHIN and not gates["B_p95_regret_ge_1.15"]
                                            and not gates["C_stable_flip"])
    gates["verdict"] = ("PLANNER HYPOTHESIS SURVIVES" if (gates["A_opportunity_rate_ge_20pct"] or gates["B_p95_regret_ge_1.15"]
                                                           or gates["C_stable_flip"])
                        else "PLANNER HYPOTHESIS FAILS FOR THIS TESTED SPACE")
    # p95 sensitivity: would the other percentile definition change gate B?
    gates["B_would_change_with_linear_p95"] = (gates["p95_regret_linear"] >= TAIL_P95) != gates["B_p95_regret_ge_1.15"]

    # per-regime and per-program breakdown
    def breakdown(keyf):
        groups = collections.defaultdict(list)
        for r in d_rows:
            groups[keyf(r)].append(r)
        out = {}
        for k, rs in groups.items():
            rg = [float(r["fixed_regret"]) for r in rs]
            out[k] = {"cells": len(rs), "median": median(rg), "p95": pctl_nearest_rank(rg, 0.95), "max": max(rg),
                      "raw_gt_1.05": sum(1 for x in rg if x > OPP_REGRET),
                      "corroborated": sum(1 for r in rs if r["opportunity"] == 1)}
        return out
    gates["by_regime"] = breakdown(lambda r: r["regime"])
    gates["by_program"] = breakdown(lambda r: r["program"])
    gates["by_size"] = {str(k): v for k, v in sorted(breakdown(lambda r: r["n"]).items())}

    winners_count = collections.Counter(r["empirical_best"] for r in d_rows)
    fixed_is_best = sum(1 for r in d_rows if r["empirical_best"] == r["fixed_plan"] or
                        float(r["fixed_regret"]) == 1.0)
    gates["empirical_best_counts"] = dict(winners_count)
    gates["cells_where_fixed_is_empirical_best"] = fixed_is_best
    gates["decisive_winner_counts"] = dict(collections.Counter(r["decisive_winner"] or "none" for r in d_rows))

    # ------------------------------------------------------------ write
    cols = sorted({k for r in cell_rows for k in r}, key=lambda k: (k.startswith("ns_"), k))
    base = ["program", "n", "regime", "units", "distinct_plans", "plans", "empirical_best", "best_ns", "fixed_plan",
            "fixed_ns", "fixed_regret", "second_best", "second_best_regret", "ranking", "twin_regret", "opportunity",
            "opp_ratio", "opp_ci_low", "opp_vs", "decisive_winner", "fixed_minflt", "best_minflt"]
    cols = base + [c for c in cols if c not in base]
    with open(os.path.join(out_dir, "cells.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        w.writerows(cell_rows)
    with open(os.path.join(out_dir, "flips.csv"), "w", newline="") as fh:
        fields = ["kind", "program", "cell1", "winner1", "cell2", "winner2", "cost_w1_in_c2", "cost_w2_in_c1", "meaningful", "stable"]
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(flips)
    with open(os.path.join(out_dir, "gates.json"), "w") as fh:
        json.dump(gates, fh, indent=2, sort_keys=True)
    write_summary(out_dir, gates, cell_rows, flips, samples, plan_info, faults, strategies, decision)
    print(json.dumps({k: v for k, v in gates.items() if not isinstance(v, dict)}, indent=2, sort_keys=True))


def fmt_us(ns):
    return f"{ns / 1000:.2f}" if ns < 1e6 else f"{ns / 1000:.0f}"


def write_summary(out_dir, gates, cell_rows, flips, samples, plan_info, faults, strategies, decision):
    L = []
    L.append("# E1 derived summary (generated by analyze.py)\n")
    L.append(f"Decision cells: {gates['decision_cells']}; replicates: {', '.join(gates['replicates'])}.\n")
    L.append("## Gates\n")
    L.append("| Statistic | Value |\n| --- | ---: |")
    for k in ["raw_cells_regret_gt_1.05", "raw_opportunity_rate", "corroborated_opportunity_cells", "corroborated_opportunity_rate",
              "median_regret", "p95_regret_nearest_rank", "p95_regret_linear", "max_regret", "share_within_5pct",
              "twin_null_median", "twin_null_p95_nearest_rank", "twin_null_max", "twin_null_share_gt_1.05",
              "stable_meaningful_flips", "flips_all_decisive_pairs"]:
        v = gates[k]
        L.append(f"| {k} | {v:.4f} |" if isinstance(v, float) else f"| {k} | {v} |")
    L.append("")
    L.append(f"A (corroborated opportunity rate >= 20 %): **{gates['A_opportunity_rate_ge_20pct']}**; "
             f"B (p95 regret >= 1.15): **{gates['B_p95_regret_ge_1.15']}**; C (stable flip): **{gates['C_stable_flip']}**; "
             f"kill condition strictly met: **{gates['kill_condition_strictly_met']}**.\n")
    L.append(f"Verdict: **{gates['verdict']}**\n")
    for title, key in (("By allocation regime", "by_regime"), ("By program", "by_program"), ("By size", "by_size")):
        L.append(f"## Fixed-policy regret {title.lower()}\n")
        L.append("| group | cells | median | p95 | max | raw > 1.05 | corroborated |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
        for k, v in gates[key].items():
            L.append(f"| {k} | {v['cells']} | {v['median']:.3f} | {v['p95']:.3f} | {v['max']:.3f} | {v['raw_gt_1.05']} | {v['corroborated']} |")
        L.append("")
    L.append("## Empirical-best plan frequency (decision cells)\n")
    L.append("| plan | cells |\n| --- | ---: |")
    for k, v in sorted(gates["empirical_best_counts"].items()):
        L.append(f"| {k} | {v} |")
    L.append("")
    L.append("## Decisive-winner frequency (decision cells)\n")
    L.append("| decisive winner | cells |\n| --- | ---: |")
    for k, v in sorted(gates["decisive_winner_counts"].items()):
        L.append(f"| {k} | {v} |")
    L.append("")
    # E1-A: time per plan across regimes for the primary programs
    for program in ("final_left", "chain3", "inter_out"):
        L.append(f"## E1-A: {program}, median ns per call by plan and regime\n")
        sizes = sorted({c[1] for c in samples if c[0] == program})
        labels = [l for l in strategies(program, sizes[0])]
        head = "| N | regime | " + " | ".join(f"{l} ({plan_info[(program, sizes[0])][l]['group']})" for l in labels) + " | faults/call (fixed) |"
        L.append(head)
        L.append("| ---: | --- | " + " | ".join("---:" for _ in labels) + " | ---: |")
        for n in sizes:
            for regime in REGIMES + SUPPLEMENTARY_REGIMES:
                c = (program, n, regime)
                if c not in samples:
                    continue
                by = samples[c]
                units = sorted(set.intersection(*[set(by[l]) for l in labels]))
                cells = [median([by[l][u] for u in units]) for l in labels]
                fixed = next(l for l, r in plan_info[(program, n)].items() if r["is_fixed"] == "1")
                L.append(f"| {n} | {regime} | " + " | ".join(f"{x:.0f}" for x in cells) + f" | {median(faults[c][fixed]):.1f} |")
        L.append("")
    L.append("## Stable meaningful flips\n")
    sf = [f for f in flips if f["meaningful"] and f["stable"]]
    if sf:
        L.append("| kind | program | cell 1 | winner 1 | cell 2 | winner 2 | cost w1 in c2 | cost w2 in c1 |\n| --- | --- | --- | --- | --- | --- | ---: | ---: |")
        for f in sf:
            L.append(f"| {f['kind']} | {f['program']} | {f['cell1']} | {f['winner1']} | {f['cell2']} | {f['winner2']} | {f['cost_w1_in_c2']} | {f['cost_w2_in_c1']} |")
    else:
        L.append("None.")
    L.append("")
    L.append(f"Decisive-winner disagreements between any two cells (any strength, any stability): {len(flips)}; "
             f"of which meaningful: {sum(1 for f in flips if f['meaningful'])}; meaningful and stable: {len(sf)}.\n")
    with open(os.path.join(out_dir, "summary.md"), "w") as fh:
        fh.write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
