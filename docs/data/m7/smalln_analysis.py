"""Small-N paired analysis (10 pairs per compiler, N = 1 and 256).
Usage (from docs/data/m7): python3 smalln_analysis.py"""
import math
import re
import statistics as st


def parse(p):
    out = {}
    for line in open(p):
        if 'MISMATCH' in line:
            raise SystemExit(f'MISMATCH in {p}')
        m = re.match(r'n=(\d+) component=(\S+) .*?median_ns=([\d.]+)', line)
        if m:
            out[(int(m[1]), m[2])] = float(m[3])
    return out


def sign_p(k, n):
    k = min(k, n - k)
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n)


for cc in ['gcc', 'clang']:
    pairs = [(parse(f'A_{cc}_smalln_run{i}.txt'), parse(f'B_{cc}_smalln_run{i}.txt')) for i in range(1, 11)]
    for n in (1, 256):
        for comp in ['native_fused_preallocated', 'lume_single_add_control', 'lume_chain_final_only',
                     'lume_chain_intermediate_output', 'lume_chain_output_after_use']:
            a = [p[0][(n, comp)] for p in pairs]
            b = [p[1][(n, comp)] for p in pairs]
            d = [y - x for x, y in zip(a, b)]
            slower = sum(x > 0 for x in d)
            print(f'{cc} n={n} {comp:32s} M6 {st.median(a):7.1f} M7 {st.median(b):7.1f} ns  '
                  f'paired M7-M6 median {st.median(d):+6.1f} ns  ratio {st.median([y / x for x, y in zip(a, b)]):.3f}  '
                  f'M7 slower in {slower}/10 (sign p={sign_p(slower, 10):.2g})')
