// E2 seam: the shared-intermediate program D = A + B, Rj = D + Cj (j = 1..k)
// under MATERIALIZE (M6 reuse on, no fusion) and RECOMPUTE (D never stored).
// Every output of both strategies must equal the independent oracle exactly
// (not merely each other), inputs must be untouched, and the executor's own
// stats must show the strategy that was asked for. The default policy must not
// recompute.
//
// Documented mutation (run once by hand, never committed): in
// execute_cpu_reference_with_policy's recompute branch, replace
//   add_add_buffers(*a, *b, *c, slot == 1)
// with
//   add_add_buffers(*a, *a, *c, slot == 1)
// i.e. recompute A + A instead of A + B. This test then fails on every
// recompute cell (output and, for i32, wrap checks). See the E2 report.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "check.hpp"
#include "execution_policy.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

namespace {

using lume::Buffer;
using lume::detail::ExecutionPolicy;
using lume::detail::ExecutionStats;

constexpr ExecutionPolicy kMaterialize{true, false, false};
constexpr ExecutionPolicy kRecompute{true, false, true};

// d_left: Rj = D + Cj, otherwise Rj = Cj + D.
lume::VerifiedProgram build(lume::ScalarType t, std::uint32_t n, std::size_t k, bool d_left = true) {
    lume::Program p;
    const auto a = p.input(t, n);
    const auto b = p.input(t, n);
    std::vector<lume::ValueId> c;
    for (std::size_t j = 0; j < k; ++j) c.push_back(p.input(t, n));
    const auto d = p.add(a, b);
    std::vector<lume::ValueId> r;
    for (std::size_t j = 0; j < k; ++j) r.push_back(d_left ? p.add(d, c[j]) : p.add(c[j], d));
    for (const auto& v : r) p.output(v);
    lume::VerifyResult v = lume::verify(std::move(p));
    if (!v.ok()) std::abort();
    return std::move(*v.program);
}

template <class T>
std::vector<T> add(const std::vector<T>& x, const std::vector<T>& y) {
    std::vector<T> out(x.size());
    lume_oracle::native_add(x, y, out);
    return out;
}

bool same(const Buffer& b, const std::vector<float>& e) {
    const auto v = b.f32_view();
    return v && lume_oracle::exactly_equal(*v, e);
}
bool same(const Buffer& b, const std::vector<std::int32_t>& e) {
    const auto v = b.i32_view();
    return v && v->size() == e.size() && std::equal(v->begin(), v->end(), e.begin());
}

template <class T>
std::vector<T> gen(std::mt19937_64& e, std::size_t n);
template <>
std::vector<float> gen<float>(std::mt19937_64& e, std::size_t n) { return lume_oracle::generate_f32(e, n); }
template <>
std::vector<std::int32_t> gen<std::int32_t>(std::mt19937_64& e, std::size_t n) { return lume_oracle::generate_i32(e, n); }

// Run both strategies on `in` = {A, B, C1..Ck} and check against the oracle.
template <class T>
void check_case(lume::ScalarType type, std::size_t k, bool d_left, const std::vector<std::vector<T>>& in, const char* what) {
    const auto n = static_cast<std::uint32_t>(in[0].size());
    std::vector<Buffer> bufs;
    for (const auto& v : in) bufs.emplace_back(v);
    const lume::VerifiedProgram vp = build(type, n, k, d_left);
    const std::vector<T> d = add(in[0], in[1]);
    std::vector<std::vector<T>> want;
    for (std::size_t j = 0; j < k; ++j) want.push_back(d_left ? add(d, in[2 + j]) : add(in[2 + j], d));

    for (int strategy = 0; strategy < 2; ++strategy) {
        ExecutionStats st;
        const lume::ExecutionResult r =
            lume::detail::execute_cpu_reference_with_policy(vp, bufs, strategy == 0 ? kMaterialize : kRecompute, &st);
        if (!LUME_CHECK(r.ok() && r.outputs.size() == k)) continue;
        for (std::size_t j = 0; j < k; ++j) {
            if (!LUME_CHECK(same(r.outputs[j], want[j]))) {
                std::fprintf(stderr, "  %s k=%zu n=%u strategy=%s output=%zu\n", what, k, n, strategy == 0 ? "M" : "R", j);
            }
        }
        const auto uk = static_cast<std::uint32_t>(k);
        ExecutionStats expect;
        if (strategy == 0) {  // D fresh, R1..R(k-1) fresh, Rk in D's storage
            expect = {uk, 1, 0, uk, 0, 0, 0};
        } else {  // k fresh results, D never stored
            expect = {uk, 0, 0, uk, 0, uk, 1};
        }
        if (!LUME_CHECK(st == expect)) {
            std::fprintf(stderr, "  %s k=%zu n=%u strategy=%s got fresh=%u inplace=%u fused=%u moves=%u copies=%u rec=%u elided=%u\n",
                         what, k, n, strategy == 0 ? "M" : "R", st.fresh_results, st.in_place_results, st.fused_pairs,
                         st.output_moves, st.output_copies, st.recomputed_consumers, st.elided_producers);
        }
        for (std::size_t q = 0; q < in.size(); ++q) LUME_CHECK(same(bufs[q], in[q]));  // inputs untouched
    }
    // Default policy never recomputes and still matches the public executor.
    ExecutionStats def;
    const lume::ExecutionResult dr = lume::detail::execute_cpu_reference_with_policy(vp, bufs, ExecutionPolicy{}, &def);
    const lume::ExecutionResult pub = lume::execute_cpu_reference(vp, bufs);
    LUME_CHECK(dr.ok() && pub.ok() && def.recomputed_consumers == 0 && def.elided_producers == 0);
    for (std::size_t j = 0; j < k && dr.ok() && pub.ok(); ++j) {
        LUME_CHECK(same(dr.outputs[j], want[j]));
        LUME_CHECK(same(pub.outputs[j], want[j]));
    }
}

template <class T>
void random_cases(lume::ScalarType type) {
    for (const std::size_t k : {1u, 2u, 3u, 4u, 8u}) {
        for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
            for (const bool d_left : {true, false}) {
                std::mt19937_64 engine(77 + n + 1000 * k);
                std::vector<std::vector<T>> in;
                for (std::size_t q = 0; q < 2 + k; ++q) in.push_back(gen<T>(engine, n));
                check_case<T>(type, k, d_left, in, "random");
            }
        }
    }
}

void f32_specials() {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float tiny = std::numeric_limits<float>::denorm_min();
    const float big = std::numeric_limits<float>::max();
    const std::vector<float> a{0.0f, -0.0f, inf, -inf, nan, tiny, big, 1.0f, 0x1p-24f, 16777216.0f, -0.0f, 3.0f};
    const std::vector<float> b{-0.0f, -0.0f, -inf, inf, 1.0f, -tiny, big, 0x1p-24f, 1.0f, 1.0f, 0.0f, nan};
    const std::vector<float> c{-0.0f, 0.0f, 1.0f, nan, 2.0f, tiny, -big, 0x1p-24f, -1.0f, 1.0f, -0.0f, 1.0f};
    for (const std::size_t k : {1u, 2u, 3u, 4u, 8u}) {
        std::vector<std::vector<float>> in{a, b};
        for (std::size_t j = 0; j < k; ++j) {
            std::vector<float> cj = c;
            std::rotate(cj.begin(), cj.begin() + static_cast<std::ptrdiff_t>(j % cj.size()), cj.end());
            in.push_back(cj);
        }
        check_case<float>(lume::f32, k, true, in, "f32 specials");
        check_case<float>(lume::f32, k, false, in, "f32 specials");
    }
    // Intermediate rounding: (2^24 + 1) + 1 differs from 2^24 + (1 + 1) in binary32.
    // (A + B) must be rounded to binary32 before + C, exactly as the oracle does.
    const std::vector<float> ra{16777216.0f, 16777216.0f, 16777216.0f};
    const std::vector<float> rb{1.0f, 1.0f, 3.0f};
    const std::vector<float> rc{1.0f, 2.0f, 1.0f};
    check_case<float>(lume::f32, 1, true, {ra, rb, rc}, "f32 rounding");
    check_case<float>(lume::f32, 2, true, {ra, rb, rc, rc}, "f32 rounding");
}

void i32_wrap() {
    constexpr std::int32_t mx = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t mn = std::numeric_limits<std::int32_t>::min();
    const std::vector<std::int32_t> a{mx, mn, mx, mn, 0, -1, 1};
    const std::vector<std::int32_t> b{1, -1, mx, mn, mn, mx, mx};
    const std::vector<std::int32_t> c{1, 1, mx, mn, mn, 1, mn};
    for (const std::size_t k : {1u, 2u, 3u, 4u, 8u}) {
        std::vector<std::vector<std::int32_t>> in{a, b};
        for (std::size_t j = 0; j < k; ++j) {
            std::vector<std::int32_t> cj = c;
            std::rotate(cj.begin(), cj.begin() + static_cast<std::ptrdiff_t>(j % cj.size()), cj.end());
            in.push_back(cj);
        }
        check_case<std::int32_t>(lume::i32, k, true, in, "i32 wrap");
        check_case<std::int32_t>(lume::i32, k, false, in, "i32 wrap");
    }
}

// Shapes the recompute rule must NOT touch: they must run exactly like M and
// stay correct.
ExecutionStats run_recompute(lume::Program p, const std::vector<Buffer>& bufs, std::vector<lume::ExecutionResult>* out) {
    lume::VerifyResult v = lume::verify(std::move(p));
    if (!v.ok()) std::abort();
    ExecutionStats st;
    out->push_back(lume::detail::execute_cpu_reference_with_policy(*v.program, bufs, kRecompute, &st));
    return st;
}

void ineligible_shapes() {
    constexpr std::uint32_t n = 17;
    std::mt19937_64 engine(5);
    const std::vector<float> a = gen<float>(engine, n), b = gen<float>(engine, n), c = gen<float>(engine, n),
                             e = gen<float>(engine, n);
    const std::vector<Buffer> bufs{Buffer(a), Buffer(b), Buffer(c), Buffer(e)};
    std::vector<lume::ExecutionResult> res;

    {  // D is also an output: it must be stored and output.
        lume::Program p;
        const auto va = p.input(lume::f32, n), vb = p.input(lume::f32, n), vc = p.input(lume::f32, n);
        p.input(lume::f32, n);
        const auto d = p.add(va, vb);
        p.output(d);
        p.output(p.add(d, vc));
        const ExecutionStats st = run_recompute(std::move(p), bufs, &res);
        LUME_CHECK(st.elided_producers == 0 && st.recomputed_consumers == 0);
        LUME_CHECK(res.back().ok() && same(res.back().outputs[0], add(a, b)) && same(res.back().outputs[1], add(add(a, b), c)));
    }
    {  // D's operand is not a caller input.
        lume::Program p;
        const auto va = p.input(lume::f32, n), vb = p.input(lume::f32, n), vc = p.input(lume::f32, n);
        p.input(lume::f32, n);
        const auto t = p.add(va, vb);
        const auto d = p.add(t, vc);
        p.output(p.add(d, va));
        const ExecutionStats st = run_recompute(std::move(p), bufs, &res);
        LUME_CHECK(res.back().ok() && same(res.back().outputs[0], add(add(add(a, b), c), a)));
        LUME_CHECK(st.recomputed_consumers <= 1);
    }
    {  // One add uses D twice (D + D).
        lume::Program p;
        const auto va = p.input(lume::f32, n), vb = p.input(lume::f32, n);
        p.input(lume::f32, n);
        p.input(lume::f32, n);
        const auto d = p.add(va, vb);
        p.output(p.add(d, d));
        const ExecutionStats st = run_recompute(std::move(p), bufs, &res);
        LUME_CHECK(st.elided_producers == 0 && st.recomputed_consumers == 0);
        LUME_CHECK(res.back().ok() && same(res.back().outputs[0], add(add(a, b), add(a, b))));
    }
    {  // Two candidates feeding one add (D + E): both must stay materialized.
        lume::Program p;
        const auto va = p.input(lume::f32, n), vb = p.input(lume::f32, n), vc = p.input(lume::f32, n),
                   ve = p.input(lume::f32, n);
        const auto d = p.add(va, vb);
        const auto q = p.add(vc, ve);
        p.output(p.add(d, q));
        const ExecutionStats st = run_recompute(std::move(p), bufs, &res);
        LUME_CHECK(st.elided_producers == 0 && st.recomputed_consumers == 0);
        LUME_CHECK(res.back().ok() && same(res.back().outputs[0], add(add(a, b), add(c, e))));
    }
    {  // Unused D (no consumer): not elided, program still valid.
        lume::Program p;
        const auto va = p.input(lume::f32, n), vb = p.input(lume::f32, n), vc = p.input(lume::f32, n);
        p.input(lume::f32, n);
        p.add(va, vb);
        p.output(vc);
        const ExecutionStats st = run_recompute(std::move(p), bufs, &res);
        LUME_CHECK(st.elided_producers == 0 && st.recomputed_consumers == 0);
        LUME_CHECK(res.back().ok() && same(res.back().outputs[0], c));
    }
}

void errors_do_not_depend_on_policy() {
    const lume::VerifiedProgram vp = build(lume::f32, 8, 2);
    const std::vector<Buffer> bad{Buffer(std::vector<float>(8)), Buffer(std::vector<float>(7)), Buffer(std::vector<float>(8)),
                                  Buffer(std::vector<float>(8))};
    for (const ExecutionPolicy& p : {kMaterialize, kRecompute}) {
        const lume::ExecutionResult r = lume::detail::execute_cpu_reference_with_policy(vp, bad, p);
        LUME_CHECK(!r.ok() && r.outputs.empty() && r.error->code == lume::ExecutionErrorCode::input_length_mismatch);
    }
}

}  // namespace

int main() {
    random_cases<float>(lume::f32);
    random_cases<std::int32_t>(lume::i32);
    f32_specials();
    i32_wrap();
    ineligible_shapes();
    errors_do_not_depend_on_policy();
    return lume_test::finish("recompute");
}
