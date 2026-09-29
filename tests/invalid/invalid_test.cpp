// Invalid representations must fail predictably. Every case here expects
// rejection; a passing test means the verifier refused the program.

#include <span>
#include <type_traits>
#include <utility>

#include "check.hpp"
#include "diagnostics.hpp"
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"

using pxir::DiagnosticCode;
using pxir::Opcode;
using pxir::OperationId;
using pxir::TypeId;
using pxir::ValueId;
using pxir_test::rejected_with;

// ---- Compile-time barriers --------------------------------------------------

// Ids are not interchangeable with each other or with raw integers.
static_assert(!std::is_convertible_v<ValueId, OperationId>);
static_assert(!std::is_convertible_v<OperationId, ValueId>);
static_assert(!std::is_convertible_v<TypeId, ValueId>);
static_assert(!std::is_convertible_v<std::uint32_t, ValueId>);
static_assert(!std::is_constructible_v<ValueId, OperationId>);

// Execution before verification is unrepresentable: a VerifiedProgram can only
// come from verify(), and the executor accepts nothing else.
static_assert(!std::is_default_constructible_v<pxir::VerifiedProgram>);
static_assert(!std::is_constructible_v<pxir::VerifiedProgram, pxir::Program>);
static_assert(!std::is_constructible_v<pxir::VerifiedProgram, const pxir::Program&>);
static_assert(!std::is_invocable_v<decltype(&pxir::execute_cpu_reference), const pxir::Program&,
                                   std::span<const pxir::Buffer>>);

namespace {

// ---- Builder-reachable invalid programs -------------------------------------

void rejects_mismatched_shapes() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 100);
    const auto b = p.input(pxir::f32, 200);
    p.output(p.add(a, b));
    PXIR_CHECK(rejected_with(pxir::verify(p), DiagnosticCode::shape_mismatch));
}

void rejects_mismatched_scalar_types() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 100);
    const auto b = p.input(pxir::i32, 100);
    p.output(p.add(a, b));
    const pxir::VerifyResult r = pxir::verify(p);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::scalar_type_mismatch));
    PXIR_CHECK(r.diagnostics.size() == 1);
}

void reports_both_scalar_and_shape_mismatch() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 100);
    const auto b = p.input(pxir::i32, 200);
    p.output(p.add(a, b));
    const pxir::VerifyResult r = pxir::verify(p);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::scalar_type_mismatch));
    PXIR_CHECK(rejected_with(r, DiagnosticCode::shape_mismatch));
}

void rejects_nonexistent_operand() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 8);
    p.output(p.add(a, ValueId{99}));
    PXIR_CHECK(rejected_with(pxir::verify(p), DiagnosticCode::invalid_operand));
}

void rejects_missing_operand() {
    pxir::Program p;
    const auto a = p.input(pxir::f32, 8);
    p.output(p.add(ValueId{}, a));
    const pxir::VerifyResult r = pxir::verify(p);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::missing_operand));
    // The builder could not derive a result type from the missing lhs.
    PXIR_CHECK(rejected_with(r, DiagnosticCode::invalid_type_id));
}

void rejects_output_of_nonexistent_value() {
    pxir::Program p;
    p.input(pxir::f32, 8);
    p.output(ValueId{42});
    PXIR_CHECK(rejected_with(pxir::verify(p), DiagnosticCode::invalid_operand));
}

void rejects_output_of_missing_value() {
    pxir::Program p;
    p.input(pxir::f32, 8);
    p.output(ValueId{});
    PXIR_CHECK(rejected_with(pxir::verify(p), DiagnosticCode::missing_operand));
}

void rejects_program_without_outputs() {
    pxir::Program p;
    p.add(p.input(pxir::f32, 8), p.input(pxir::f32, 8));
    PXIR_CHECK(rejected_with(pxir::verify(p), DiagnosticCode::no_outputs));
    PXIR_CHECK(rejected_with(pxir::verify(pxir::Program{}), DiagnosticCode::no_outputs));
}

void rejects_zero_length() {
    pxir::Program p;
    p.output(p.input(pxir::f32, 0));
    PXIR_CHECK(rejected_with(pxir::verify(p), DiagnosticCode::zero_length));
}

void rejects_unknown_scalar_type() {
    pxir::Program p;
    p.output(p.input(static_cast<pxir::ScalarType>(0), 4));
    p.output(p.input(static_cast<pxir::ScalarType>(7), 4));
    const pxir::VerifyResult r = pxir::verify(p);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::invalid_scalar_type));
    PXIR_CHECK(r.diagnostics.size() == 2);
}

void byte_size_rejects_unknown_scalar() {
    PXIR_CHECK(!pxir::byte_size(pxir::Type{static_cast<pxir::ScalarType>(9), 4}).has_value());
    if constexpr (sizeof(std::size_t) >= 8) {
        PXIR_CHECK(pxir::byte_size(pxir::Type{pxir::f32, 0xffffffffu}) == std::size_t{0xffffffffu} * 4u);
    } else {
        PXIR_CHECK(!pxir::byte_size(pxir::Type{pxir::f32, 0xffffffffu}).has_value());
    }
}

// ---- Structurally malformed storage (unreachable through the builder) -------

// A well-formed vector add, as raw storage, for targeted corruption.
pxir::ProgramStorage valid_storage() {
    pxir::ProgramStorage s;
    s.types = {pxir::Type{pxir::f32, 4}};
    s.values = {pxir::Value{TypeId{0}, OperationId{0}}, pxir::Value{TypeId{0}, OperationId{1}},
                pxir::Value{TypeId{0}, OperationId{2}}};
    s.operations = {
        pxir::Operation{Opcode::input, {}, ValueId{0}},
        pxir::Operation{Opcode::input, {}, ValueId{1}},
        pxir::Operation{Opcode::add, {ValueId{0}, ValueId{1}}, ValueId{2}},
        pxir::Operation{Opcode::output, {ValueId{2}, ValueId{}}, ValueId{}},
    };
    return s;
}

pxir::VerifyResult verify_storage(pxir::ProgramStorage s) { return pxir::verify(pxir::Program::from_storage(std::move(s))); }

void baseline_storage_is_valid() { PXIR_CHECK(verify_storage(valid_storage()).ok()); }

void rejects_value_with_nonexistent_type() {
    auto s = valid_storage();
    s.values[1].type = TypeId{5};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::invalid_type_id));
}

void rejects_value_with_nonexistent_definer() {
    auto s = valid_storage();
    s.values[0].definer = OperationId{17};
    const pxir::VerifyResult r = verify_storage(s);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::invalid_definer));
    PXIR_CHECK(rejected_with(r, DiagnosticCode::result_mismatch));  // op 0 still claims %0

    s = valid_storage();
    s.values[0].definer = OperationId{};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::invalid_definer));
}

void rejects_definer_mismatch() {
    auto s = valid_storage();
    s.values[1].definer = OperationId{0};  // op 0 names %0 as its result, not %1
    const pxir::VerifyResult r = verify_storage(s);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::definer_mismatch));
    PXIR_CHECK(rejected_with(r, DiagnosticCode::result_mismatch));  // op 1 claims %1
}

void rejects_use_of_own_result() {
    auto s = valid_storage();
    s.operations[2].operands[1] = ValueId{2};  // %2 = add %0, %2
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::use_before_definition));
}

void rejects_forward_reference() {
    auto s = valid_storage();
    // Move the add after the output: output now uses %2 before op 3 defines it.
    std::swap(s.operations[2], s.operations[3]);
    s.values[2].definer = OperationId{3};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::use_before_definition));
}

void rejects_unknown_opcode() {
    auto s = valid_storage();
    s.operations.push_back(pxir::Operation{static_cast<Opcode>(0), {}, ValueId{}});
    s.operations.push_back(pxir::Operation{static_cast<Opcode>(200), {}, ValueId{}});
    const pxir::VerifyResult r = verify_storage(s);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::invalid_opcode));
    PXIR_CHECK(r.diagnostics.size() == 2);
}

void rejects_operand_on_input() {
    auto s = valid_storage();
    s.operations[1].operands[0] = ValueId{0};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::unexpected_operand));
}

void rejects_second_operand_on_output() {
    auto s = valid_storage();
    s.operations[3].operands[1] = ValueId{0};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::unexpected_operand));
}

void rejects_result_on_output() {
    auto s = valid_storage();
    s.operations[3].result = ValueId{2};
    const pxir::VerifyResult r = verify_storage(s);
    PXIR_CHECK(rejected_with(r, DiagnosticCode::unexpected_result));
}

void rejects_missing_and_nonexistent_result() {
    auto s = valid_storage();
    s.operations[2].result = ValueId{};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::missing_result));

    s = valid_storage();
    s.operations[0].result = ValueId{50};
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::invalid_result));
}

void rejects_result_defined_elsewhere() {
    auto s = valid_storage();
    s.operations[2].result = ValueId{0};  // add claims input 0's value
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::result_mismatch));
}

void rejects_add_result_type_mismatch() {
    auto s = valid_storage();
    s.types.push_back(pxir::Type{pxir::f32, 8});
    s.values[2].type = TypeId{1};  // %2: f32[8] = add f32[4], f32[4]
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::result_type_mismatch));
}

void rejects_malformed_type_table_entry() {
    auto s = valid_storage();
    s.types[0].length = 0;
    PXIR_CHECK(rejected_with(verify_storage(s), DiagnosticCode::zero_length));
}

}  // namespace

int main() {
    rejects_mismatched_shapes();
    rejects_mismatched_scalar_types();
    reports_both_scalar_and_shape_mismatch();
    rejects_nonexistent_operand();
    rejects_missing_operand();
    rejects_output_of_nonexistent_value();
    rejects_output_of_missing_value();
    rejects_program_without_outputs();
    rejects_zero_length();
    rejects_unknown_scalar_type();
    byte_size_rejects_unknown_scalar();

    baseline_storage_is_valid();
    rejects_value_with_nonexistent_type();
    rejects_value_with_nonexistent_definer();
    rejects_definer_mismatch();
    rejects_use_of_own_result();
    rejects_forward_reference();
    rejects_unknown_opcode();
    rejects_operand_on_input();
    rejects_second_operand_on_output();
    rejects_result_on_output();
    rejects_missing_and_nonexistent_result();
    rejects_result_defined_elsewhere();
    rejects_add_result_type_mismatch();
    rejects_malformed_type_table_entry();
    return pxir_test::finish("invalid");
}
