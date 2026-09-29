// Verifier acceptance and diagnostic determinism.

#include <cstdio>

#include "check.hpp"
#include "pxir/verify/verifier.hpp"

namespace {

pxir::Program vector_add(pxir::ScalarType scalar, std::uint32_t n) {
    pxir::Program p;
    const auto a = p.input(scalar, n);
    const auto b = p.input(scalar, n);
    p.output(p.add(a, b));
    return p;
}

void accepts_f32_vector_add() {
    const pxir::VerifyResult r = pxir::verify(vector_add(pxir::f32, 1024));
    PXIR_CHECK(r.ok());
    PXIR_CHECK(r.diagnostics.empty());
    for (const auto& d : r.diagnostics) std::fprintf(stderr, "  %s\n", d.message.c_str());
}

void accepts_i32_vector_add() {
    PXIR_CHECK(pxir::verify(vector_add(pxir::i32, 1)).ok());
}

void accepts_chained_adds_and_multiple_outputs() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 3);
    const auto b = p.input(pxir::f32, 3);
    const auto c = p.add(a, b);
    const auto d = p.add(c, a);
    p.output(d);
    p.output(a);  // passing an input through is allowed
    PXIR_CHECK(pxir::verify(p).ok());
}

void verified_program_preserves_ir() {
    const pxir::Program original = vector_add(pxir::f32, 16);
    const pxir::VerifyResult r = pxir::verify(original);
    if (!PXIR_CHECK(r.ok())) return;
    const pxir::ProgramStorage& s = r.program->program().storage();
    PXIR_CHECK(s.operations.size() == original.storage().operations.size());
    PXIR_CHECK(s.values.size() == original.storage().values.size());
}

void diagnostics_are_deterministic() {
    const auto build = [] {
        pxir::Program p;
        const auto a = p.input(pxir::f32, 100);
        const auto b = p.input(pxir::i32, 200);
        p.add(a, b);
        p.add(a, pxir::ValueId{99});
        return p;
    };
    const pxir::VerifyResult first = pxir::verify(build());
    const pxir::VerifyResult second = pxir::verify(build());
    PXIR_CHECK(!first.ok());
    if (!PXIR_CHECK(first.diagnostics.size() == second.diagnostics.size())) return;
    for (std::size_t i = 0; i < first.diagnostics.size(); ++i) {
        PXIR_CHECK(first.diagnostics[i].code == second.diagnostics[i].code);
        PXIR_CHECK(first.diagnostics[i].message == second.diagnostics[i].message);
    }
}

void diagnostic_messages_are_actionable() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 100);
    const auto b = p.input(pxir::f32, 200);
    p.output(p.add(a, b));
    const pxir::VerifyResult r = pxir::verify(p);
    if (!PXIR_CHECK(r.diagnostics.size() == 1)) return;
    const pxir::Diagnostic& d = r.diagnostics[0];
    PXIR_CHECK(d.code == pxir::DiagnosticCode::shape_mismatch);
    PXIR_CHECK(d.operation == pxir::OperationId{2});
    PXIR_CHECK(d.message ==
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
    return pxir_test::finish("verifier");
}
