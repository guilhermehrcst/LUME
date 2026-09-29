// M2: an output moves an executor-owned buffer only when it is the value's
// last use; every other output copies. These tests check that results,
// ordering and borrowed inputs are unchanged by that rule.

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "check.hpp"
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"
#include "pxir_oracle/oracle.hpp"

using pxir::Buffer;

namespace {

constexpr std::uint32_t n = 1000;

struct Inputs {
    std::vector<float> a;
    std::vector<float> b;
};

Inputs make_inputs() {
    std::mt19937_64 engine(2024);
    Inputs in;
    in.a = pxir_oracle::generate_f32(engine, n);
    in.b = pxir_oracle::generate_f32(engine, n);
    return in;
}

std::vector<float> add(const std::vector<float>& x, const std::vector<float>& y) {
    std::vector<float> out(x.size());
    pxir_oracle::native_add(x, y, out);
    return out;
}

// Verifies, executes, and checks that the caller's buffers are unchanged.
pxir::ExecutionResult run(pxir::Program program, const Inputs& in) {
    pxir::VerifyResult verified = pxir::verify(std::move(program));
    if (!PXIR_CHECK(verified.ok())) return {};
    const std::vector<Buffer> buffers{Buffer(in.a), Buffer(in.b)};
    pxir::ExecutionResult r = pxir::execute_cpu_reference(*verified.program, buffers);
    PXIR_CHECK(r.ok());
    PXIR_CHECK(pxir_oracle::exactly_equal(*buffers[0].as_f32(), in.a));
    PXIR_CHECK(pxir_oracle::exactly_equal(*buffers[1].as_f32(), in.b));
    return r;
}

bool output_equals(const pxir::ExecutionResult& r, std::size_t k, const std::vector<float>& expected) {
    return k < r.outputs.size() && r.outputs[k].as_f32() != nullptr &&
           pxir_oracle::exactly_equal(*r.outputs[k].as_f32(), expected);
}

// A: output C, its single and final use -> moved.
void single_owned_output() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto a = p.input(pxir::f32, n);
    const auto b = p.input(pxir::f32, n);
    p.output(p.add(a, b));
    const auto r = run(std::move(p), in);
    PXIR_CHECK(r.outputs.size() == 1);
    PXIR_CHECK(output_equals(r, 0, add(in.a, in.b)));
}

// B: output C twice -> first copies, second moves; both identical.
void same_owned_value_output_twice() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto c = p.add(p.input(pxir::f32, n), p.input(pxir::f32, n));
    p.output(c);
    p.output(c);
    const auto r = run(std::move(p), in);
    const auto expected = add(in.a, in.b);
    PXIR_CHECK(r.outputs.size() == 2);
    PXIR_CHECK(output_equals(r, 0, expected));
    PXIR_CHECK(output_equals(r, 1, expected));
}

// C: output C, then C is read by a later add -> the output must copy.
void output_before_later_computational_use() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto a = p.input(pxir::f32, n);
    const auto b = p.input(pxir::f32, n);
    const auto c = p.add(a, b);
    p.output(c);
    p.output(p.add(c, a));
    const auto r = run(std::move(p), in);
    const auto expected_c = add(in.a, in.b);
    PXIR_CHECK(r.outputs.size() == 2);
    PXIR_CHECK(output_equals(r, 0, expected_c));
    PXIR_CHECK(output_equals(r, 1, add(expected_c, in.a)));
}

// D: outputs of inputs are copies; the caller's buffers stay untouched
// (checked in run()), including when an input is output last.
void input_passthrough() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto a = p.input(pxir::f32, n);
    const auto b = p.input(pxir::f32, n);
    p.output(a);
    p.output(p.add(a, b));
    p.output(b);
    const auto r = run(std::move(p), in);
    PXIR_CHECK(r.outputs.size() == 3);
    PXIR_CHECK(output_equals(r, 0, in.a));
    PXIR_CHECK(output_equals(r, 1, add(in.a, in.b)));
    PXIR_CHECK(output_equals(r, 2, in.b));
}

// E: C is consumed only by D = C + C; output D -> exact.
void chained_computation() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto c = p.add(p.input(pxir::f32, n), p.input(pxir::f32, n));
    p.output(p.add(c, c));
    const auto r = run(std::move(p), in);
    const auto expected_c = add(in.a, in.b);
    PXIR_CHECK(r.outputs.size() == 1);
    PXIR_CHECK(output_equals(r, 0, add(expected_c, expected_c)));
}

// F: the move path for i32-owned buffers, including a copy-then-move pair.
void i32_owned_outputs() {
    std::mt19937_64 engine(99);
    const auto x = pxir_oracle::generate_i32(engine, n);
    const auto y = pxir_oracle::generate_i32(engine, n);
    std::vector<std::int32_t> expected(n);
    pxir_oracle::native_add(x, y, expected);

    pxir::Program p;
    const auto c = p.add(p.input(pxir::i32, n), p.input(pxir::i32, n));
    p.output(c);
    p.output(c);
    pxir::VerifyResult verified = pxir::verify(std::move(p));
    if (!PXIR_CHECK(verified.ok())) return;
    const std::vector<Buffer> buffers{Buffer(x), Buffer(y)};
    const auto r = pxir::execute_cpu_reference(*verified.program, buffers);
    if (!PXIR_CHECK(r.ok()) || !PXIR_CHECK(r.outputs.size() == 2)) return;
    PXIR_CHECK(r.outputs[0].as_i32() != nullptr && *r.outputs[0].as_i32() == expected);
    PXIR_CHECK(r.outputs[1].as_i32() != nullptr && *r.outputs[1].as_i32() == expected);
    PXIR_CHECK(*buffers[0].as_i32() == x);
    PXIR_CHECK(*buffers[1].as_i32() == y);
}

// G: outputs appear exactly in IR order, whether each is a move or a copy.
void multiple_output_ordering() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto a = p.input(pxir::f32, n);
    const auto b = p.input(pxir::f32, n);
    const auto c = p.add(a, b);  // a + b
    const auto d = p.add(c, b);  // (a + b) + b
    const auto e = p.add(a, a);  // a + a
    p.output(e);                 // move (last use of e)
    p.output(c);                 // move (c's last reader, d, already ran)
    p.output(b);                 // copy (input)
    p.output(d);                 // copy (read again below)
    p.output(d);                 // move
    const auto r = run(std::move(p), in);
    const auto ec = add(in.a, in.b);
    const auto ed = add(ec, in.b);
    PXIR_CHECK(r.outputs.size() == 5);
    PXIR_CHECK(output_equals(r, 0, add(in.a, in.a)));
    PXIR_CHECK(output_equals(r, 1, ec));
    PXIR_CHECK(output_equals(r, 2, in.b));
    PXIR_CHECK(output_equals(r, 3, ed));
    PXIR_CHECK(output_equals(r, 4, ed));
}

// The same verified program executes repeatedly with identical results: the
// move affects only the ExecutionResult of that call, never the program.
void repeated_execution_is_stable() {
    const Inputs in = make_inputs();
    pxir::Program p;
    const auto c = p.add(p.input(pxir::f32, n), p.input(pxir::f32, n));
    p.output(c);
    p.output(c);
    pxir::VerifyResult verified = pxir::verify(std::move(p));
    if (!PXIR_CHECK(verified.ok())) return;
    const std::vector<Buffer> buffers{Buffer(in.a), Buffer(in.b)};
    const auto expected = add(in.a, in.b);
    for (int i = 0; i < 3; ++i) {
        const auto r = pxir::execute_cpu_reference(*verified.program, buffers);
        PXIR_CHECK(r.ok());
        PXIR_CHECK(output_equals(r, 0, expected));
        PXIR_CHECK(output_equals(r, 1, expected));
    }
}

}  // namespace

int main() {
    single_owned_output();
    same_owned_value_output_twice();
    output_before_later_computational_use();
    input_passthrough();
    chained_computation();
    i32_owned_outputs();
    multiple_output_ordering();
    repeated_execution_is_stable();
    return pxir_test::finish("output_move");
}
