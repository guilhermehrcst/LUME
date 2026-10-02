"""Aggregate M7 A/B benchmark output.

A_* files: canonical main f636bdf (M6 runtime). B_* files: M7. Same benchmark
source. For each (config, n, component): median over runs of each run's median.
Usage: python3 aggregate.py [config-prefix ...]   (run from docs/data/m7)
"""
import collections
import glob
import re
import statistics as st
import sys

LINE = re.compile(r'n=(\d+) component=(\S+) .*?median_ns=([\d.]+).*?minflt_per_op=([\d.]+)')
COMPONENTS = [
    'native_fused_preallocated', 'native_two_pass_preallocated',
    'lume_single_add_control', 'lume_chain_final_only',
    'lume_chain_intermediate_output', 'lume_chain_output_after_use',
]


def load(pattern):
    out = collections.defaultdict(list)
    files = sorted(glob.glob(pattern))
    for f in files:
        for line in open(f):
            if 'MISMATCH' in line:
                print('MISMATCH in', f)
            m = LINE.match(line)
            if m:
                out[(int(m[1]), m[2])].append((float(m[3]), float(m[4])))
    return out, len(files)


def table(cfg):
    a, na = load(f'A_{cfg}*.txt')
    b, nb = load(f'B_{cfg}*.txt')
    print(f'## {cfg}  (files A={na} B={nb}; median of per-run medians, ns; faults = median minflt/op)')
    print('n | component | M6 (A) | M7 (B) | M7/M6 | M6 faults | M7 faults')
    for n in sorted({k[0] for k in a}):
        for c in COMPONENTS:
            if (n, c) not in a or (n, c) not in b:
                continue
            am = st.median(x[0] for x in a[(n, c)])
            bm = st.median(x[0] for x in b[(n, c)])
            af = st.median(x[1] for x in a[(n, c)])
            bf = st.median(x[1] for x in b[(n, c)])
            print(f'{n} | {c} | {am:.1f} | {bm:.1f} | {bm / am:.3f} | {af:.1f} | {bf:.1f}')
    print()


if __name__ == '__main__':
    for cfg in sys.argv[1:] or ['gcc_default_run', 'gcc_default_reverse', 'gcc_t1_run', 'gcc_t2_run',
                                'clang_default_run', 'clang_t1_run']:
        table(cfg)
