// Verifier acceptance and diagnostic determinism.

#include <cstdio>

#include "check.hpp"
#include "lume/verify/verifier.hpp"

namespace {

lume::Program vector_add(lume::ScalarType scalar, std::uint32_t n) {
    lume::Program p;
    const auto a = p.input(scalar, n);
    const auto b = p.input(scalar, n);
    p.output(p.add(a, b));
    return p;
}

void accepts_f32_vector_add() {
    const lume::VerifyResult r = lume::verify(vector_add(lume::f32, 1024));
    LUME_CHECK(r.ok());
    LUME_CHECK(r.diagnostics.empty());
    for (const auto& d : r.diagnostics) std::fprintf(stderr, "  %s\n", d.message.c_str());
}

void accepts_i32_vector_add() {
    LUME_CHECK(lume::verify(vector_add(lume::i32, 1)).ok());
}

void accepts_chained_adds_and_multiple_outputs() {
    lume::Program p;
    const auto a = p.input(lume::f32, 3);
    const auto b = p.input(lume::f32, 3);
    const auto c = p.add(a, b);
    const auto d = p.add(c, a);
    p.output(d);
    p.output(a);  // passing an input through is allowed
    LUME_CHECK(lume::verify(p).ok());
}

void verified_program_preserves_ir() {
    const lume::Program original = vector_add(lume::f32, 16);
    const lume::VerifyResult r = lume::verify(original);
    if (!LUME_CHECK(r.ok())) return;
    const lume::ProgramStorage& s = r.program->program().storage();
    LUME_CHECK(s.operations.size() == original.storage().operations.size());
    LUME_CHECK(s.values.size() == original.storage().values.size());
}

void diagnostics_are_deterministic() {
    const auto build = [] {
        lume::Program p;
        const auto a = p.input(lume::f32, 100);
        const auto b = p.input(lume::i32, 200);
        p.add(a, b);
        p.add(a, lume::ValueId{99});
        return p;
    };
    const lume::VerifyResult first = lume::verify(build());
    const lume::VerifyResult second = lume::verify(build());
    LUME_CHECK(!first.ok());
    if (!LUME_CHECK(first.diagnostics.size() == second.diagnostics.size())) return;
    for (std::size_t i = 0; i < first.diagnostics.size(); ++i) {
        LUME_CHECK(first.diagnostics[i].code == second.diagnostics[i].code);
        LUME_CHECK(first.diagnostics[i].message == second.diagnostics[i].message);
    }
}

void diagnostic_messages_are_actionable() {
    lume::Program p;
    const auto a = p.input(lume::f32, 100);
    const auto b = p.input(lume::f32, 200);
    p.output(p.add(a, b));
    const lume::VerifyResult r = lume::verify(p);
    if (!LUME_CHECK(r.diagnostics.size() == 1)) return;
    const lume::Diagnostic& d = r.diagnostics[0];
    LUME_CHECK(d.code == lume::DiagnosticCode::shape_mismatch);
    LUME_CHECK(d.operation == lume::OperationId{2});
    LUME_CHECK(d.message ==
               "error[shape_mismatch] op 2 (add): cannot add %0: f32[100] and %1: f32[200] (lengths differ)");
}

}  // namespace

int main() {
    accepts_f32_vector_add();
    accepts_i32_vector_add();
    accepts_chained_adds_and_multiple_outputs();
    verified_program_preserves_ir();
    diagnostics_are_deterministic();
    diagnostic_messages_are_actionable();
    return lume_test::finish("verifier");
}
