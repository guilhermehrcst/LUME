#!/usr/bin/env python3
"""Summarize the tight vector loops of a disassembled executor object (x86-64 or AArch64).

usage: codegen_report.py <objdump.txt> <label>

Input: output of `objdump -d -C --no-show-raw-insn` (x86: add `-M intel`) of cpu_reference.o.
A "tight loop" is a backward-branch range of at most MAXBODY instructions that contains a
vector add. Reported per loop: instruction count, vector adds, vector loads, vector stores,
index step. Also reports global facts: AVX/NEON-SVE usage, non-temporal stores, prefetches,
string instructions, libc mem* calls. Observation only: nothing is inferred about caches.
The AArch64 parser is untested on real AArch64 output until run on such a host.
"""
import re
import sys

MAXBODY = 24
X86_ADD = re.compile(r"^(v?addps|v?paddd)\b")
A64_ADD = re.compile(r"^(fadd|add)\s+v\d+\.(4s|2s|2d|8h|16b)")
X86_LD = re.compile(r"^(v?movups|v?movdqu|v?movaps|v?movdqa|movq|movss)\s+(x|y|z)mm\d+,\s*\w+ PTR")
X86_ADDMEM = re.compile(r"^(v?addps|v?paddd|addss)\s+(x|y|z)mm\d+,\s*\w+ PTR")
X86_ST = re.compile(r"^(v?movups|v?movdqu|v?movaps|v?movdqa|movlps|movss)\s+\w+ PTR")
A64_LD = re.compile(r"^(ldr|ldp|ld1\w*|ldur)\s+[qvd]\d+")
A64_ST = re.compile(r"^(str|stp|st1\w*|stur)\s+[qvd]\d+")


def parse(path):
    t = open(path).read()
    funcs = []
    for f in re.split(r"\n(?=[0-9a-f]{8,16} <)", t):
        m = re.match(r"[0-9a-f]{8,16} <(.*)>:", f)
        if not m:
            continue
        ins = []
        for l in f.split("\n")[1:]:
            mm = re.match(r"\s*([0-9a-f]+):\s*(.*)", l)
            if mm:
                ins.append((int(mm.group(1), 16), re.sub(r"\s*<[^>]*>", "", mm.group(2).strip())))
        funcs.append((m.group(1), ins))
    return funcs


def main():
    path, label = sys.argv[1:3]
    funcs = parse(path)
    arch = "aarch64" if any(re.match(r"(ldp|stp|fadd)\b", s) for _, ins in funcs for _, s in ins[:200]) else "x86"
    print(f"## {label} ({arch})\n")
    allins = [s for _, ins in funcs for _, s in ins]
    wide = sum(bool(re.search(r"\b(y|z)mm\d+", s)) for s in allins) if arch == "x86" else 0
    print(f"- ymm/zmm (AVX/AVX-512) instructions: {wide}")
    memcall = re.compile(r"(call|bl)\s+.*(memcpy|memset|memmove)")
    print("- non-temporal stores: %d; prefetches: %d; string ops (rep movs/stos): %d; calls to memcpy/memset/memmove: %d" % (
        sum(bool(re.match(r"(v?movnt|stnp)", s)) for s in allins),
        sum(bool(re.match(r"(prefetch|prfm)", s)) for s in allins),
        sum(bool(re.match(r"rep ", s)) for s in allins),
        sum(bool(memcall.search(s)) for s in allins)))
    print("\n| function | loop body instrs | vector adds | vector loads (x86: incl. memory-operand adds; AArch64: ldp counts 2) | vector stores | index step |")
    print("| --- | ---: | ---: | ---: | ---: | --- |")
    seen = set()
    for name, ins in funcs:
        if ".cold" in name:
            continue
        for i, (a, s) in enumerate(ins):
            m = re.match(r"(j\w+|b\.\w+|b|cbnz|cbz|tbnz|tbz)\s+(?:\w+,\s*)*(?:0x)?([0-9a-f]+)\b", s)
            if not m:
                continue
            try:
                tgt = int(m.group(2), 16)
            except ValueError:
                continue
            if tgt > a:
                continue
            body = [x for x in ins if tgt <= x[0] <= a]
            if len(body) > MAXBODY:
                continue
            txt = [s2 for _, s2 in body]
            va = sum(bool((X86_ADD if arch == "x86" else A64_ADD).match(x)) for x in txt)
            if not va:
                continue
            if arch == "x86":
                ld = sum(bool(X86_LD.match(x)) for x in txt) + sum(bool(X86_ADDMEM.match(x)) for x in txt)
                stv = sum(bool(X86_ST.match(x)) for x in txt)
                step = [re.match(r"add\s+\w+,0x(\w+)$", x).group(1) for x in txt if re.match(r"add\s+r\w+,0x(10|20|8)$", x)]
            else:
                # ldp/stp move two vector registers per instruction
                ld = sum((2 if x.startswith("ldp") else 1) for x in txt if A64_LD.match(x))
                stv = sum((2 if x.startswith("stp") else 1) for x in txt if A64_ST.match(x))
                step = [x for x in txt if re.match(r"(add|sub)\s+x\d+,\s*x\d+,\s*#", x)][:1]
            key = (name, tgt)
            if key in seen:
                continue
            seen.add(key)
            short = re.sub(r"\(.*", "", name)
            short = short if "execute_cpu" not in name else "execute_cpu_reference_with_policy (inlined kernel)"
            print(f"| {short[:70]} @{hex(tgt)} | {len(body)} | {va} | {ld} | {stv} | {step[:1]} |")


if __name__ == "__main__":
    main()
