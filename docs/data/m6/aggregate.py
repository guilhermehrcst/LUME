import re,sys,glob,statistics as st,collections
D='/home/user/PXIR/docs/data/m6/'
def load(pat):
    out=collections.defaultdict(list)  # (n,comp)->[(med,flt)]
    for f in sorted(glob.glob(D+pat)):
        for l in open(f):
            m=re.match(r'n=(\d+) component=(\S+) .*?median_ns=([\d.]+).*?minflt_per_op=([\d.]+)',l)
            if m: out[(int(m[1]),m[2])].append((float(m[3]),float(m[4])))
            if 'MISMATCH' in l: print('MISMATCH in',f)
    return out
def table(cfg, comps, sizes=None):
    A=load('A_'+cfg+'*.txt'); B=load('B_'+cfg+'*.txt')
    ns=sorted({k[0] for k in A})
    if sizes: ns=[n for n in ns if n in sizes]
    print(f'## {cfg}  (runs A={len(next(iter(A.values())))} B={len(next(iter(B.values())))})')
    print('n | component | M5 med ns | M6 med ns | M6/M5 | M5 flt | M6 flt')
    for n in ns:
        for c in comps:
            a=A.get((n,c)); b=B.get((n,c))
            if not a or not b: continue
            am=st.median(x[0] for x in a); bm=st.median(x[0] for x in b)
            print(f'{n} | {c} | {am:.0f} | {bm:.0f} | {bm/am:.3f} | {st.median(x[1] for x in a):.1f} | {st.median(x[1] for x in b):.1f}')
comps=['pxir_single_add_control','pxir_chain_final_only','pxir_chain_intermediate_output','pxir_chain_output_after_use']
if __name__=='__main__':
    for cfg in sys.argv[1:]:
        table(cfg,comps+['native_fused_preallocated','native_two_pass_preallocated'])
