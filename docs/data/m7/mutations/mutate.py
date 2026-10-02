# M7 mutation driver (run from the repository root). Reads a pristine copy of
# the executor (src/runtime/cpu_reference.cpp.orig, made by the operator before
# mutating), writes the mutated src/runtime/cpu_reference.cpp. After each
# mutation the tests are built in a separate tree and run, and the pristine
# file is copied back. Usage: python3 mutate.py A|B|C1|C2
import sys
p='src/runtime/cpu_reference.cpp'
s=open('src/runtime/cpu_reference.cpp.orig').read()
m=sys.argv[1]
def rep(old,new):
    global s
    assert old in s, (m, old[:60])
    s=s.replace(old,new,1)
if m=='A':  # reassociation in the integrated fused kernel
    rep("""            const T t = add(a[i], b[i]);
            r[i] = add(c[i], t);""","""            r[i] = add(add(c[i], a[i]), b[i]);  // MUTATION A""")
    rep("""            const T t = add(a[i], b[i]);
            r[i] = add(t, c[i]);""","""            r[i] = add(a[i], add(b[i], c[i]));  // MUTATION A""")
elif m=='B':  # ignore last use of T
    rep("""        if (last_use[t.index()] != OperationId{static_cast<std::uint32_t>(k + 1)}) return std::nullopt;
""","""        // MUTATION B: last_use[T] not checked
""")
elif m=='C1':  # consumer searched forward; executor still skips op i + 1
    rep("""        const Operation& second = s.operations[k + 1];
        if (second.opcode != Opcode::add) return std::nullopt;
        const ValueId t = first.result;""","""        const ValueId t = first.result;
        std::size_t j = k + 1;  // MUTATION C1: search forward for T's consumer
        while (j < s.operations.size() && s.operations[j].operands[0] != t && s.operations[j].operands[1] != t) ++j;
        if (j >= s.operations.size()) return std::nullopt;
        const Operation& second = s.operations[j];
        if (second.opcode != Opcode::add) return std::nullopt;""")
    rep("""        if (last_use[t.index()] != OperationId{static_cast<std::uint32_t>(k + 1)}) return std::nullopt;""","""        if (last_use[t.index()] != OperationId{static_cast<std::uint32_t>(j)}) return std::nullopt;""")
    rep("""        if (reusable(second.operands[1 - slot], k + 1)) return std::nullopt;""","""        if (reusable(second.operands[1 - slot], j)) return std::nullopt;""")
    rep("""                    const Operation& second = s.operations[i + 1];""","""                    std::size_t j = i + 1;  // MUTATION C1
                    while (s.operations[j].operands[0] != op.result && s.operations[j].operands[1] != op.result) ++j;
                    const Operation& second = s.operations[j];""")
elif m=='C2':  # consumer searched forward; executor correctly skips the consumer later
    rep("""        const Operation& second = s.operations[k + 1];
        if (second.opcode != Opcode::add) return std::nullopt;
        const ValueId t = first.result;""","""        const ValueId t = first.result;
        std::size_t j = k + 1;  // MUTATION C2: search forward for T's consumer
        while (j < s.operations.size() && s.operations[j].operands[0] != t && s.operations[j].operands[1] != t) ++j;
        if (j >= s.operations.size()) return std::nullopt;
        const Operation& second = s.operations[j];
        if (second.opcode != Opcode::add) return std::nullopt;""")
    rep("""        if (last_use[t.index()] != OperationId{static_cast<std::uint32_t>(k + 1)}) return std::nullopt;""","""        if (last_use[t.index()] != OperationId{static_cast<std::uint32_t>(j)}) return std::nullopt;""")
    rep("""        if (reusable(second.operands[1 - slot], k + 1)) return std::nullopt;""","""        if (reusable(second.operands[1 - slot], j)) return std::nullopt;""")
    rep("""    ExecutionResult result;
    std::size_t next_input = 0;
    for (std::size_t i = 0; i < s.operations.size(); ++i) {
        const Operation& op = s.operations[i];""","""    ExecutionResult result;
    std::size_t next_input = 0;
    std::vector<bool> done(s.operations.size(), false);  // MUTATION C2
    for (std::size_t i = 0; i < s.operations.size(); ++i) {
        if (done[i]) continue;
        const Operation& op = s.operations[i];""")
    rep("""                    const Operation& second = s.operations[i + 1];""","""                    std::size_t j = i + 1;  // MUTATION C2
                    while (s.operations[j].operands[0] != op.result && s.operations[j].operands[1] != op.result) ++j;
                    const Operation& second = s.operations[j];
                    done[j] = true;""")
    rep("""                    ++i;  // operation i + 1 has been executed
                    break;""","""                    break;  // MUTATION C2: the consumer is skipped via done[]""")
else:
    raise SystemExit('unknown')
open(p,'w').write(s)
