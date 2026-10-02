"""Paired A/B analysis for M7 (A = canonical main / M6 runtime, B = M7).

Pair i is A_<cfg>_run<i>.txt and B_<cfg>_run<i>.txt, run back to back (the
order alternates between pairs). For every pair the script uses ratios of
per-run medians, so slow drift of the shared host cancels within a pair:

  paired      B/A of the same component
  normalized  (B component / B native fused) / (A component / A native fused):
              each run's own native fused loop (identical code in A and B)
              serves as a speed reference
  vs_fused    B component / B native fused (how close M7 gets to the bound)
  saving      A final-only time - B final-only time, per pair (microseconds)

Reported: median, interquartile range, and a two-sided sign test (how many
pairs have ratio < 1). Usage: python3 paired_analysis.py <dir> <cfg> [...]
"""
import glob
import math
import os
import re
import statistics as st
import sys

LINE = re.compile(r'n=(\d+) component=(\S+) .*?median_ns=([\d.]+).*?minflt_per_op=([\d.]+)')
FUSED = 'native_fused_preallocated'
TWO_PASS = 'native_two_pass_preallocated'
LUME = ['lume_single_add_control', 'lume_chain_final_only', 'lume_chain_intermediate_output',
        'lume_chain_output_after_use']


def parse(path):
    out = {}
    for line in open(path):
        if 'MISMATCH' in line:
            raise SystemExit(f'MISMATCH in {path}')
        m = LINE.match(line)
        if m:
            out[(int(m[1]), m[2])] = (float(m[3]), float(m[4]))
    return out


def sign_p(below, n):
    k = min(below, n - below)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n)


def summary(xs):
    xs = sorted(xs)
    q = st.quantiles(xs, n=4) if len(xs) >= 2 else [xs[0], xs[0], xs[0]]
    below = sum(x < 1 for x in xs)
    return f'{st.median(xs):.3f} [IQR {q[0]:.3f}-{q[2]:.3f}] below1={below}/{len(xs)} p={sign_p(below, len(xs)):.2g}'


def analyse(directory, cfg):
    pairs = []
    for a in sorted(glob.glob(os.path.join(directory, f'A_{cfg}*.txt'))):
        b = a.replace(os.sep + 'A_', os.sep + 'B_')
        if os.path.exists(b):
            pairs.append((parse(a), parse(b)))
    print(f'## {cfg}: {len(pairs)} pairs')
    sizes = sorted({k[0] for k in pairs[0][0] if k[1] == FUSED})
    for n in sizes:
        print(f'n={n}')
        for comp in [FUSED, TWO_PASS]:
            print(f'  {comp:32s} paired B/A (identical code: noise floor) {summary([b[(n, comp)][0] / a[(n, comp)][0] for a, b in pairs])}')
        for comp in LUME:
            paired = [b[(n, comp)][0] / a[(n, comp)][0] for a, b in pairs]
            norm = [(b[(n, comp)][0] / b[(n, FUSED)][0]) / (a[(n, comp)][0] / a[(n, FUSED)][0]) for a, b in pairs]
            fa = st.median(a[(n, comp)][1] for a, _ in pairs)
            fb = st.median(b[(n, comp)][1] for _, b in pairs)
            print(f'  {comp:32s} paired B/A {summary(paired)}')
            print(f'  {"":32s} normalized {summary(norm)}  faults/call A={fa:.1f} B={fb:.1f}')
        diff = [(a[(n, 'lume_chain_final_only')][0] - b[(n, 'lume_chain_final_only')][0]) / 1000 for a, b in pairs]
        q = st.quantiles(diff, n=4) if len(diff) >= 2 else [diff[0]] * 3
        print(f'  final-only paired saving A-B: median {st.median(diff):.1f} us [IQR {q[0]:.1f}-{q[2]:.1f}]')
        for side, idx in (('A (M6)', 0), ('B (M7)', 1)):
            r_fused = [p[idx][(n, 'lume_chain_final_only')][0] / p[idx][(n, FUSED)][0] for p in pairs]
            r_two = [p[idx][(n, 'lume_chain_final_only')][0] / p[idx][(n, TWO_PASS)][0] for p in pairs]
            med = st.median(p[idx][(n, 'lume_chain_final_only')][0] for p in pairs)
            print(f'  final-only {side}: median {med / 1000:.1f} us; / native fused {st.median(r_fused):.3f}; / native two-pass {st.median(r_two):.3f}')
    print()


if __name__ == '__main__':
    d = sys.argv[1]
    for cfg in sys.argv[2:]:
        analyse(d, cfg)
