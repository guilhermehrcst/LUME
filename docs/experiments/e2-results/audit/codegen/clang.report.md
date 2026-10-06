## clang (x86)

- ymm/zmm (AVX/AVX-512) instructions: 0
- non-temporal stores: 0; prefetches: 0; string ops (rep movs/stos): 0; calls to memcpy/memset/memmove: 0

| function | loop body instrs | vector adds | vector loads (incl. memory-operand adds) | vector stores | index step (bytes or elems) |
| --- | ---: | ---: | ---: | ---: | --- |
| execute_cpu_reference_with_policy (inlined kernel) @0x328d | 11 | 2 | 4 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x32f6 | 11 | 2 | 4 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x3b07 | 9 | 2 | 2 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x3b3b | 9 | 2 | 2 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x3bad | 11 | 2 | 4 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x3bf5 | 11 | 2 | 4 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x3c45 | 11 | 2 | 4 | 2 | ['8'] |
| execute_cpu_reference_with_policy (inlined kernel) @0x3c8d | 11 | 2 | 4 | 2 | ['8'] |
| lume:: @0x70e0 | 15 | 4 | 6 | 2 | ['8'] |
| lume:: @0x7170 | 15 | 4 | 6 | 2 | ['8'] |
| lume::OwnedArray<int> lume:: @0x7260 | 15 | 4 | 6 | 2 | ['8'] |
| lume::OwnedArray<int> lume:: @0x72f0 | 15 | 4 | 6 | 2 | ['8'] |
