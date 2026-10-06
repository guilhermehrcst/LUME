## gcc (x86)

- ymm/zmm (AVX/AVX-512) instructions: 0
- non-temporal stores: 0; prefetches: 0; string ops (rep movs/stos): 0; calls to memcpy/memset/memmove: 0

| function | loop body instrs | vector adds | vector loads (incl. memory-operand adds) | vector stores | index step (bytes or elems) |
| --- | ---: | ---: | ---: | ---: | --- |
| lume:: @0x748 | 9 | 2 | 3 | 1 | ['10'] |
| lume:: @0x8fb | 9 | 2 | 3 | 1 | ['10'] |
| lume:: @0xa08 | 9 | 2 | 3 | 1 | ['10'] |
| lume:: @0xb1a | 9 | 2 | 3 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x6f15 | 7 | 1 | 2 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x7099 | 7 | 1 | 2 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x74c9 | 7 | 1 | 2 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x74fd | 20 | 1 | 2 | 0 | [] |
| execute_cpu_reference_with_policy (inlined kernel) @0x7721 | 7 | 1 | 2 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x7751 | 18 | 1 | 4 | 2 | [] |
| execute_cpu_reference_with_policy (inlined kernel) @0x782a | 6 | 1 | 1 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x78e4 | 7 | 1 | 2 | 1 | ['10'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x799f | 7 | 1 | 2 | 1 | ['10'] |
