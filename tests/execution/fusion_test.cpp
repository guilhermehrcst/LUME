// M7: single-use intermediate fusion. Every case checks values against the
// independent oracle (with a materialized intermediate), that caller inputs
// are unchanged, the number of result-sized heap allocations per execution,
// and which add pairs the real executor fused (internal observation seam:
// fusion is otherwise unobservable by design).

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "check.hpp"
#include "fusion_observer.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

namespace {

// Counts operator new / new[] calls of at least `g_threshold` bytes while
// armed (same approach as the M6 test). kN = 4099 elements is 16396 bytes,
// far above any executor bookkeeping allocation.
bool g_armed = false;
std::size_t g_threshold = 0;
std::size_t g_big_allocations = 0;

void* counted_alloc(std::size_t size) {
    if (g_armed && size >= g_threshold) ++g_big_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using lume::Buffer;
using F = std::vector<float>;
using I = std::vector<std::int32_t>;
using Ops = std::vector<std::uint32_t>;

constexpr std::uint32_t kN = 4099;

// Mirrors the executor: f32 pairs are fused only where float expressions have
// no excess precision (FLT_EVAL_METHOD == 0). Elsewhere they must not be
// fused; values and allocation counts are the same either way.
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
constexpr bool kF32Fused = true;
#else
constexpr bool kF32Fused = false;
#endif

// The fused pairs expected for an f32 program whose eligible pairs start at `ops`.
Ops f32_fused(Ops ops) { return kF32Fused ? ops : Ops{}; }

lume::VerifiedProgram verified(lume::Program p) {
    lume::VerifyResult r = lume::verify(std::move(p));
    return std::move(*r.program);
}

// Index of the operation that defines v.
std::uint32_t op_of(const lume::Program& p, lume::ValueId v) { return p.storage().values[v.index()].definer.value; }

struct Run {
    lume::ExecutionResult result;
    std::size_t big_allocations = 0;
    Ops fused;
};

Run run(const lume::VerifiedProgram& program, const std::vector<Buffer>& inputs, std::size_t elements = kN) {
    Run r;
    r.fused.reserve(16);  // the seam's push_back then does not allocate while counting
    g_threshold = elements * sizeof(float);
    g_big_allocations = 0;
    g_armed = true;
    r.result = lume::detail::execute_cpu_reference_observed(program, inputs, &r.fused);
    g_armed = false;
    r.big_allocations = g_big_allocations;
    return r;
}

F sum(const F& x, const F& y) {
    F out(x.size());
    lume_oracle::native_add(x, y, out);
    return out;
}
I sum(const I& x, const I& y) {
    I out(x.size());
    lume_oracle::native_add(x, y, out);
    return out;
}

bool is(const Buffer& b, const F& expected) {
    const auto v = b.f32_view();
    return v && lume_oracle::exactly_equal(*v, expected);
}
bool is(const Buffer& b, const I& expected) {
    const auto v = b.i32_view();
    return v && v->size() == expected.size() && std::equal(v->begin(), v->end(), expected.begin());
}

// Finite f32 values whose three-term sums usually need rounding, so a change
// of grouping is visible (the oracle's generate_f32 values are multiples of
// 2^-23 in [-1, 1) and almost always sum exactly).
F rounding_sensitive(std::mt19937_64& engine, std::size_t n) {
    F out(n);
    for (float& x : out) {
        const std::uint64_t r = engine();
        const auto significand = static_cast<float>((r & 0xffffffu) | 0x800000u);
        const int exponent = static_cast<int>((r >> 24) % 41u) - 20 - 23;
        x = std::ldexp((r >> 63) != 0 ? -significand : significand, exponent);
    }
    return out;
}

struct Data {
    F a, b, c, f, g;
};

Data make_data(std::uint32_t seed) {
    std::mt19937_64 engine(seed);
    Data d;
    d.a = rounding_sensitive(engine, kN);
    d.b = rounding_sensitive(engine, kN);
    d.c = rounding_sensitive(engine, kN);
    d.f = rounding_sensitive(engine, kN);
    d.g = rounding_sensitive(engine, kN);
    return d;
}

std::vector<Buffer> inputs_of(const Data& d) {
    return {Buffer(d.a), Buffer(d.b), Buffer(d.c), Buffer(d.f), Buffer(d.g)};
}

lume::Program five_inputs(lume::ValueId (&v)[5]) {
    lume::Program p;
    for (auto& x : v) x = p.input(lume::f32, kN);
    return p;
}

void inputs_unchanged(const std::vector<Buffer>& in, const Data& d) {
    LUME_CHECK(is(in[0], d.a) && is(in[1], d.b) && is(in[2], d.c) && is(in[3], d.f) && is(in[4], d.g));
}

// 31. D = A + B; E = D + C; output E. Fused; one result allocation, as in M6.
void primary_left() {
    const Data d = make_data(1);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(dd, v[2]));
    const Ops expected = f32_fused({op_of(p, dd)});
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    LUME_CHECK(is(r.result.outputs[0], sum(sum(d.a, d.b), d.c)));
    LUME_CHECK(r.fused == expected);
    LUME_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// 32. E = C + D: the intermediate is the right operand; IR order C + (A + B).
void primary_right() {
    const Data d = make_data(2);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(v[2], dd));
    const Ops expected = f32_fused({op_of(p, dd)});
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    LUME_CHECK(is(r.result.outputs[0], sum(d.c, sum(d.a, d.b))));
    LUME_CHECK(r.fused == expected);
    LUME_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// 33. D = A + B; output D; E = D + C; output E. Not adjacent and D is
// observable: no fusion; M6 behavior (D, the copy of D; E reuses D).
void intermediate_output() {
    const Data d = make_data(3);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(dd);
    p.output(p.add(dd, v[2]));
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    const F expected_d = sum(d.a, d.b);
    LUME_CHECK(is(r.result.outputs[0], expected_d));
    LUME_CHECK(is(r.result.outputs[1], sum(expected_d, d.c)));
    LUME_CHECK(r.fused.empty());
    LUME_CHECK(r.big_allocations == 2);
    inputs_unchanged(in, d);
}

// 34. D = A + B; E = D + C; output D; output E. last_use[D] is the later
// output: no fusion; D and E both exist.
void later_output() {
    const Data d = make_data(4);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    const auto e = p.add(dd, v[2]);
    p.output(dd);
    p.output(e);
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    const F expected_d = sum(d.a, d.b);
    LUME_CHECK(is(r.result.outputs[0], expected_d));
    LUME_CHECK(is(r.result.outputs[1], sum(expected_d, d.c)));
    LUME_CHECK(r.fused.empty());
    LUME_CHECK(r.big_allocations == 2);
    inputs_unchanged(in, d);
}

// 35. D = A + B; X = C + F; E = D + G; output E; output X. D is single-use
// but its consumer is not the next operation: no fusion (no search).
void non_adjacent() {
    const Data d = make_data(5);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    const auto x = p.add(v[2], v[3]);
    p.output(p.add(dd, v[4]));
    p.output(x);
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    LUME_CHECK(is(r.result.outputs[0], sum(sum(d.a, d.b), d.g)));
    LUME_CHECK(is(r.result.outputs[1], sum(d.c, d.f)));
    LUME_CHECK(r.fused.empty());
    LUME_CHECK(r.big_allocations == 2);  // D, X; E reuses D
    inputs_unchanged(in, d);
}

// 36. E = D + D: the intermediate appears twice; not fused in M7 (M6 reuses D).
void repeated_operand() {
    const Data d = make_data(6);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(dd, dd));
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    const F expected_d = sum(d.a, d.b);
    LUME_CHECK(is(r.result.outputs[0], sum(expected_d, expected_d)));
    LUME_CHECK(r.fused.empty());
    LUME_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// 37. The first add of a candidate pair has an M6-reusable operand:
// X = A + B; Z = C + F; T = X + C; R = T + G; output R; output Z.
// At T, X is owned and at its last use, so M6 computes T in X's storage.
// Fusing (T, R) would allocate R instead (3 allocations, not 2): rejected.
void first_add_reusable_source() {
    const Data d = make_data(7);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto x = p.add(v[0], v[1]);
    const auto z = p.add(v[2], v[3]);
    const auto t = p.add(x, v[2]);
    p.output(p.add(t, v[4]));
    p.output(z);
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    LUME_CHECK(is(r.result.outputs[0], sum(sum(sum(d.a, d.b), d.c), d.g)));
    LUME_CHECK(is(r.result.outputs[1], sum(d.c, d.f)));
    LUME_CHECK(r.fused.empty());
    LUME_CHECK(r.big_allocations == 2);  // X, Z; T and R reuse X's storage
    inputs_unchanged(in, d);
}

// 38. The second add's other operand is M6-reusable:
// X = A + B; Z = C + F; T = A + C; R = T + X (or X + T); output R; output Z.
// X is owned and at its last use at R: rejected, both operand orders.
void second_other_reusable() {
    for (const bool t_is_rhs : {false, true}) {
        const Data d = make_data(t_is_rhs ? 9u : 8u);
        const auto in = inputs_of(d);
        lume::ValueId v[5];
        lume::Program p = five_inputs(v);
        const auto x = p.add(v[0], v[1]);
        const auto z = p.add(v[2], v[3]);
        const auto t = p.add(v[0], v[2]);
        p.output(t_is_rhs ? p.add(x, t) : p.add(t, x));
        p.output(z);
        const Run r = run(verified(std::move(p)), in);
        if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
        const F expected_x = sum(d.a, d.b);
        const F expected_t = sum(d.a, d.c);
        LUME_CHECK(is(r.result.outputs[0], t_is_rhs ? sum(expected_x, expected_t) : sum(expected_t, expected_x)));
        LUME_CHECK(is(r.result.outputs[1], sum(d.c, d.f)));
        LUME_CHECK(r.fused.empty());
        LUME_CHECK(r.big_allocations == 3);  // X, Z, T; R reuses T (lhs) or X (lhs)
        inputs_unchanged(in, d);
    }
}

// 39. Caller inputs as every operand, including the same input twice:
// D = A + A; E = D + A. Fused; A is never written.
void borrowed_same_input() {
    const Data d = make_data(10);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[0]);
    p.output(p.add(dd, v[0]));
    const Ops expected = f32_fused({op_of(p, dd)});
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
    LUME_CHECK(is(r.result.outputs[0], sum(sum(d.a, d.a), d.a)));
    LUME_CHECK(r.fused == expected);
    LUME_CHECK(r.big_allocations == 1);
    inputs_unchanged(in, d);
}

// The fused result is an ordinary owned value afterwards:
// D = A + B; E = D + C; output E (copy: read later); F = E + G; output F.
void fused_result_used_later() {
    const Data d = make_data(11);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    const auto e = p.add(dd, v[2]);
    p.output(e);
    p.output(p.add(e, v[4]));
    const Ops expected = f32_fused({op_of(p, dd)});
    const Run r = run(verified(std::move(p)), in);
    if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 2)) return;
    const F expected_e = sum(sum(d.a, d.b), d.c);
    LUME_CHECK(is(r.result.outputs[0], expected_e));
    LUME_CHECK(is(r.result.outputs[1], sum(expected_e, d.g)));
    LUME_CHECK(r.fused == expected);
    LUME_CHECK(r.big_allocations == 2);  // E, the output copy of E; F reuses E
    inputs_unchanged(in, d);
}

// 42. V1 = A + B; V2 = V1 + C; V3 = V2 + F [; V4 = V3 + G]; output last.
// Greedy pairwise: (V1, V2) is fused; V3 is the M6 in-place add on V2 (its
// first operand V2 is M6-reusable, so (V3, V4) is rejected and V4 is M6 too).
void longer_chain() {
    for (const bool four : {false, true}) {
        const Data d = make_data(four ? 13u : 12u);
        const auto in = inputs_of(d);
        lume::ValueId v[5];
        lume::Program p = five_inputs(v);
        const auto v1 = p.add(v[0], v[1]);
        const auto v2 = p.add(v1, v[2]);
        auto last = p.add(v2, v[3]);
        if (four) last = p.add(last, v[4]);
        p.output(last);
        const Ops expected = f32_fused({op_of(p, v1)});
        const Run r = run(verified(std::move(p)), in);
        if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
        F expected_out = sum(sum(sum(d.a, d.b), d.c), d.f);
        if (four) expected_out = sum(expected_out, d.g);
        LUME_CHECK(is(r.result.outputs[0], expected_out));
        LUME_CHECK(r.fused == expected);
        LUME_CHECK(r.big_allocations == 1);
        inputs_unchanged(in, d);
    }
}

// 41. The same VerifiedProgram repeatedly: bit-identical results (checksum),
// inputs unchanged, same fused pairs every time.
void repeated_execution() {
    const Data d = make_data(14);
    const auto in = inputs_of(d);
    lume::ValueId v[5];
    lume::Program p = five_inputs(v);
    const auto dd = p.add(v[0], v[1]);
    p.output(p.add(v[2], dd));
    const Ops expected = f32_fused({op_of(p, dd)});
    const lume::VerifiedProgram program = verified(std::move(p));
    const F expected_e = sum(d.c, sum(d.a, d.b));
    const std::uint64_t checksum = lume_oracle::fnv1a(expected_e);
    for (int k = 0; k < 4; ++k) {
        const Run r = run(program, in);
        if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
        LUME_CHECK(is(r.result.outputs[0], expected_e));
        LUME_CHECK(lume_oracle::fnv1a(*r.result.outputs[0].f32_view()) == checksum);
        LUME_CHECK(r.fused == expected);
        LUME_CHECK(r.big_allocations == 1);
        inputs_unchanged(in, d);
    }
}

// 18. Rounding-order sentinels through the executor, both consumer sides:
// s1 = (1, 2^-24, 2^-24): (A+B)+C = 1, A+(B+C) = 1 + 2^-23;
// s2 = (2^-24, 2^-24, 1): (A+B)+C = 1 + 2^-23, A+(B+C) = (C+A)+B = 1.
void rounding_sentinels() {
    const F a{1.0f, 0x1p-24f};
    const F b{0x1p-24f, 0x1p-24f};
    const F c{0x1p-24f, 1.0f};
    const std::vector<Buffer> in{Buffer(a), Buffer(b), Buffer(c)};
    for (const bool t_is_rhs : {false, true}) {
        lume::Program p;
        const auto ia = p.input(lume::f32, 2);
        const auto ib = p.input(lume::f32, 2);
        const auto ic = p.input(lume::f32, 2);
        const auto t = p.add(ia, ib);
        p.output(t_is_rhs ? p.add(ic, t) : p.add(t, ic));
        const Ops expected = f32_fused({op_of(p, t)});
        const Run r = run(verified(std::move(p)), in, 2);
        if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
        LUME_CHECK(r.fused == expected);
        LUME_CHECK(is(r.result.outputs[0], F{1.0f, 1.0f + 0x1p-23f}));
    }
}

// Edge lengths, oracle-generated data, both sides.
void edge_lengths() {
    for (const std::uint32_t n : {1u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 33u, 4099u}) {
        std::mt19937_64 engine(900 + n);
        const F a = lume_oracle::generate_f32(engine, n);
        const F b = lume_oracle::generate_f32(engine, n);
        const F c = lume_oracle::generate_f32(engine, n);
        const std::vector<Buffer> in{Buffer(a), Buffer(b), Buffer(c)};
        for (const bool t_is_rhs : {false, true}) {
            lume::Program p;
            const auto ia = p.input(lume::f32, n);
            const auto ib = p.input(lume::f32, n);
            const auto ic = p.input(lume::f32, n);
            const auto t = p.add(ia, ib);
            p.output(t_is_rhs ? p.add(ic, t) : p.add(t, ic));
            const Ops expected = f32_fused({op_of(p, t)});
            const Run r = run(verified(std::move(p)), in, n);
            if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
            LUME_CHECK(r.fused == expected);
            LUME_CHECK(is(r.result.outputs[0], t_is_rhs ? sum(c, sum(a, b)) : sum(sum(a, b), c)));
            if (!LUME_CHECK(is(in[0], a) && is(in[1], b) && is(in[2], c))) std::fprintf(stderr, "  n=%u\n", n);
        }
    }
}

// f32 special values (signed zero, infinities, NaNs with distinct payloads,
// signaling NaN, subnormals, overflow). NaN results follow the existing
// policy: any NaN matches any NaN.
void f32_specials() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float den = std::numeric_limits<float>::denorm_min();
    constexpr float fmax = std::numeric_limits<float>::max();
    const float qnan1 = std::bit_cast<float>(0x7fc00001u);
    const float qnan2 = std::bit_cast<float>(0xffc00002u);
    const float snan = std::bit_cast<float>(0x7f800001u);
    const F a{-0.0f, -0.0f, inf, inf, -inf, qnan1, 1.0f, snan, qnan1, den, -den, fmax, -fmax};
    const F b{-0.0f, 0.0f, -inf, 1.0f, -inf, 1.0f, qnan2, 2.0f, qnan2, den, den, fmax, 1.0f};
    const F c{-0.0f, -0.0f, 0.0f, -inf, inf, 3.0f, 0.0f, 1.0f, snan, -den, -den, -fmax, fmax};
    const std::vector<Buffer> in{Buffer(a), Buffer(b), Buffer(c)};
    for (const bool t_is_rhs : {false, true}) {
        lume::Program p;
        const auto ia = p.input(lume::f32, 13);
        const auto ib = p.input(lume::f32, 13);
        const auto ic = p.input(lume::f32, 13);
        const auto t = p.add(ia, ib);
        p.output(t_is_rhs ? p.add(ic, t) : p.add(t, ic));
        const Run r = run(verified(std::move(p)), in, 13);
        if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
        LUME_CHECK(r.fused.size() == (kF32Fused ? 1u : 0u));
        LUME_CHECK(is(r.result.outputs[0], t_is_rhs ? sum(c, sum(a, b)) : sum(sum(a, b), c)));
        LUME_CHECK(std::signbit((*r.result.outputs[0].f32_view())[0]));  // (-0 + -0) + -0 = -0
    }
    // Inputs, including NaN payloads, are untouched (compare bit patterns).
    const auto a_bits = *in[0].f32_view();
    bool same = true;
    for (std::size_t i = 0; i < a.size(); ++i) {
        same = same && std::bit_cast<std::uint32_t>(a_bits[i]) == std::bit_cast<std::uint32_t>(a[i]);
    }
    LUME_CHECK(same);
}

// 40. i32: fused pairs compute wrap(wrap(A + B) + C), both sides; negative
// controls behave as for f32.
void i32_cases() {
    constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    {
        const I a{hi, lo, hi, -1, 0, 1, hi, lo, -1, lo};
        const I b{1, -1, hi, 1, lo, -1, hi, lo, -1, 0};
        const I c{hi, lo, 2, lo, lo, 0, hi, lo, 1, -1};
        const I expected_left{-1, -1, 0, lo, 0, 0, hi - 2, lo, -1, hi};  // by hand
        LUME_CHECK(sum(sum(a, b), c) == expected_left);                 // the oracle agrees
        const std::vector<Buffer> in{Buffer(a), Buffer(b), Buffer(c)};
        for (const bool t_is_rhs : {false, true}) {
            lume::Program p;
            const auto ia = p.input(lume::i32, 10);
            const auto ib = p.input(lume::i32, 10);
            const auto ic = p.input(lume::i32, 10);
            const auto t = p.add(ia, ib);
            p.output(t_is_rhs ? p.add(ic, t) : p.add(t, ic));
            const Ops expected{op_of(p, t)};  // i32: always fused
            const Run r = run(verified(std::move(p)), in, 10);
            if (!LUME_CHECK(r.result.ok() && r.result.outputs.size() == 1)) return;
            LUME_CHECK(r.fused == expected);
            LUME_CHECK(is(r.result.outputs[0], t_is_rhs ? sum(c, sum(a, b)) : expected_left));
            LUME_CHECK(is(in[0], a) && is(in[1], b) && is(in[2], c));
        }
    }
    std::mt19937_64 engine(77);
    const I x = lume_oracle::generate_i32(engine, kN);
    const I y = lume_oracle::generate_i32(engine, kN);
    const I z = lume_oracle::generate_i32(engine, kN);
    const std::vector<Buffer> in{Buffer(x), Buffer(y), Buffer(z)};
    const I xy = sum(x, y);
    // Primary (fused), later output (not fused), repeated operand (not fused).
    for (int shape = 0; shape < 3; ++shape) {
        lume::Program p;
        const auto ia = p.input(lume::i32, kN);
        const auto ib = p.input(lume::i32, kN);
        const auto ic = p.input(lume::i32, kN);
        const auto t = p.add(ia, ib);
        const auto e = shape == 2 ? p.add(t, t) : p.add(t, ic);
        if (shape == 1) p.output(t);
        p.output(e);
        const std::uint32_t first = op_of(p, t);
        const Run r = run(verified(std::move(p)), in);
        if (!LUME_CHECK(r.result.ok())) return;
        if (shape == 0) {
            LUME_CHECK(r.fused == Ops{first} && r.big_allocations == 1);
            LUME_CHECK(r.result.outputs.size() == 1 && is(r.result.outputs[0], sum(xy, z)));
        } else if (shape == 1) {
            LUME_CHECK(r.fused.empty() && r.big_allocations == 2);
            LUME_CHECK(r.result.outputs.size() == 2 && is(r.result.outputs[0], xy) &&
                       is(r.result.outputs[1], sum(xy, z)));
        } else {
            LUME_CHECK(r.fused.empty() && r.big_allocations == 1);
            LUME_CHECK(r.result.outputs.size() == 1 && is(r.result.outputs[0], sum(xy, xy)));
        }
        LUME_CHECK(is(in[0], x) && is(in[1], y) && is(in[2], z));
    }
}

}  // namespace

int main() {
    std::printf("fusion: f32 pairs %s (FLT_EVAL_METHOD), i32 pairs fused\n", kF32Fused ? "fused" : "not fused");
    primary_left();
    primary_right();
    intermediate_output();
    later_output();
    non_adjacent();
    repeated_operand();
    first_add_reusable_source();
    second_other_reusable();
    borrowed_same_input();
    fused_result_used_later();
    longer_chain();
    repeated_execution();
    rounding_sentinels();
    edge_lengths();
    f32_specials();
    i32_cases();
    return lume_test::finish("fusion");
}
