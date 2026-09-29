// IR construction: ids, tables, type interning, debug dump, footprint.

#include <cstdio>
#include <string>

#include "check.hpp"
#include "pxir/ir/dump.hpp"
#include "pxir/ir/program.hpp"

namespace {

void builds_vector_add_with_deterministic_ids() {
    pxir::Program program;
    const pxir::ValueId a = program.input(pxir::f32, 1024);
    const pxir::ValueId b = program.input(pxir::f32, 1024);
    const pxir::ValueId c = program.add(a, b);
    const pxir::OperationId out = program.output(c);

    PXIR_CHECK(a == pxir::ValueId{0});
    PXIR_CHECK(b == pxir::ValueId{1});
    PXIR_CHECK(c == pxir::ValueId{2});
    PXIR_CHECK(out == pxir::OperationId{3});

    const pxir::ProgramStorage& s = program.storage();
    PXIR_CHECK(s.operations.size() == 4);
    PXIR_CHECK(s.values.size() == 3);
    PXIR_CHECK(s.types.size() == 1);  // f32[1024] interned once

    PXIR_CHECK(s.operations[0].opcode == pxir::Opcode::input);
    PXIR_CHECK(s.operations[2].opcode == pxir::Opcode::add);
    PXIR_CHECK(s.operations[2].operands[0] == a);
    PXIR_CHECK(s.operations[2].operands[1] == b);
    PXIR_CHECK(s.operations[2].result == c);
    PXIR_CHECK(s.operations[3].opcode == pxir::Opcode::output);
    PXIR_CHECK(s.operations[3].operands[0] == c);
    PXIR_CHECK(!s.operations[3].operands[1].is_valid());
    PXIR_CHECK(!s.operations[3].result.is_valid());

    PXIR_CHECK(s.values[2].definer == pxir::OperationId{2});
    PXIR_CHECK(s.values[2].type == pxir::TypeId{0});
    PXIR_CHECK((s.types[0] == pxir::Type{pxir::f32, 1024}));
}

void repeated_construction_is_identical() {
    const auto build = [] {
        pxir::Program p;
        p.output(p.add(p.input(pxir::f32, 8), p.input(pxir::f32, 8)));
        return pxir::to_debug_string(p);
    };
    PXIR_CHECK(build() == build());
}

void represents_i32_and_distinct_types() {
    pxir::Program program;
    const pxir::ValueId x = program.input(pxir::i32, 16);
    const pxir::ValueId y = program.input(pxir::f32, 16);
    const pxir::ValueId z = program.input(pxir::i32, 32);
    const pxir::ProgramStorage& s = program.storage();
    PXIR_CHECK(s.types.size() == 3);
    PXIR_CHECK((s.types[s.values[x.index()].type.index()] == pxir::Type{pxir::i32, 16}));
    PXIR_CHECK((s.types[s.values[y.index()].type.index()] == pxir::Type{pxir::f32, 16}));
    PXIR_CHECK((s.types[s.values[z.index()].type.index()] == pxir::Type{pxir::i32, 32}));
}

void debug_dump_matches_expected_text() {
    pxir::Program program;
    const auto a = program.input(pxir::f32, 1024);
    const auto b = program.input(pxir::f32, 1024);
    program.output(program.add(a, b));
    const std::string expected =
        "%0 = input f32[1024]\n"
        "%1 = input f32[1024]\n"
        "%2 = add %0, %1\n"
        "output %2\n";
    const std::string actual = pxir::to_debug_string(program);
    if (!PXIR_CHECK(actual == expected)) std::fprintf(stderr, "actual:\n%s", actual.c_str());
}

void debug_dump_tolerates_malformed_program() {
    pxir::ProgramStorage s;
    s.operations.push_back(pxir::Operation{pxir::Opcode::add, {pxir::ValueId{7}, pxir::ValueId{}}, pxir::ValueId{}});
    s.operations.push_back(pxir::Operation{static_cast<pxir::Opcode>(0), {}, pxir::ValueId{}});
    s.operations.push_back(pxir::Operation{pxir::Opcode::input, {}, pxir::ValueId{9}});
    const std::string actual = pxir::to_debug_string(pxir::Program::from_storage(s));
    PXIR_CHECK(actual == "<none> = add %7, <none>\n<invalid-opcode 0>\n%9 = input <invalid-type>\n");
}

void footprint_counts_table_bytes() {
    pxir::Program program;
    program.output(program.add(program.input(pxir::f32, 4), program.input(pxir::f32, 4)));
    const pxir::StorageFootprint f = pxir::storage_footprint(program);
    const std::size_t expected = 1 * sizeof(pxir::Type) + 3 * sizeof(pxir::Value) + 4 * sizeof(pxir::Operation);
    PXIR_CHECK(f.used_bytes == expected);
    PXIR_CHECK(f.reserved_bytes >= f.used_bytes);
}

void ids_default_to_invalid() {
    PXIR_CHECK(!pxir::ValueId{}.is_valid());
    PXIR_CHECK(!pxir::OperationId::invalid().is_valid());
    PXIR_CHECK(pxir::TypeId{0}.is_valid());
}

}  // namespace

int main() {
    builds_vector_add_with_deterministic_ids();
    repeated_construction_is_identical();
    represents_i32_and_distinct_types();
    debug_dump_matches_expected_text();
    debug_dump_tolerates_malformed_program();
    footprint_counts_table_bytes();
    ids_default_to_invalid();
    return pxir_test::finish("ir");
}
