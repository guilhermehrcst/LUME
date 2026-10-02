"""Counts vector/scalar add, load/store and call instructions per add_add instantiation in an objdump -d listing.
Usage: python3 codegen_count.py disasm_integrated_probe_g++.txt disasm_integrated_probe_clang++.txt"""
import re,sys,collections
for path in sys.argv[1:]:
    funcs=[]; cur=None
    for line in open(path):
        m=re.match(r'^([0-9a-f]+) <(.*)>:$',line.rstrip())
        if m:
            name=m.group(2)
            t='f32' if 'add_add<float' in name else ('i32' if 'add_add<int' in name else '?')
            tag='.cold' if '.cold' in name else ''
            cur=[f'add_add<{t}>{tag}',[]]; funcs.append(cur); continue
        if cur and re.match(r'^\s+[0-9a-f]+:\s',line):
            parts=line.split('\t')
            if len(parts)>=2: cur[1].append(parts[1].strip())
    print('=====',path.split('/')[-1])
    for name,ins in funcs:
        c=collections.Counter(i.split()[0] for i in ins if i)
        calls=[i for i in ins if i.startswith('call')]
        keys=['movups','movdqu','addps','paddd','movss','addss']
        print(f'  {name}: '+' '.join(f'{k}={c[k]}' for k in keys)+f' calls={len(calls)}')
        for cl in calls:
            print('     ', re.sub(r'call\s+[0-9a-f]+\s+<([^(>]*).*',r'call \1',cl)[:100])
