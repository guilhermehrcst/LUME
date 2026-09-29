// M4 Phase A: OwnedArray<T> in isolation. The single-write indexed kernels
// below are the loops M4 proposes for add_buffers. Every element is written
// before any read; results are compared with the oracle. Copies must be deep
// and independent; moves must transfer ownership and leave the source empty.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "check.hpp"
#include "pxir/runtime/owned_array.hpp"
#include "pxir_oracle/oracle.hpp"

using pxir::OwnedArray;

static_assert(std::is_nothrow_move_constructible_v<OwnedArray<float>>);
static_assert(std::is_nothrow_move_constructible_v<OwnedArray<std::int32_t>>);
static_assert(std::is_nothrow_move_assignable_v<OwnedArray<std::int32_t>>);

namespace {

std::int32_t wrapping_add(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

OwnedArray<float> add_f32(std::span<const float> a, std::span<const float> b) {
    auto c = OwnedArray<float>::for_overwrite(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) c[i] = a[i] + b[i];
    return c;
}

OwnedArray<std::int32_t> add_i32(std::span<const std::int32_t> a, std::span<const std::int32_t> b) {
    auto c = OwnedArray<std::int32_t>::for_overwrite(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) c[i] = wrapping_add(a[i], b[i]);
    return c;
}

bool same_i32(std::span<const std::int32_t> x, std::span<const std::int32_t> y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i] != y[i]) return false;
    }
    return true;
}

constexpr std::uint32_t edge_lengths[] = {1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 4099};

void f32_edge_lengths() {
    for (const std::uint32_t n : edge_lengths) {
        std::mt19937_64 engine(n);
        const auto a = pxir_oracle::generate_f32(engine, n);
        const auto b = pxir_oracle::generate_f32(engine, n);
        std::vector<float> expected(n);
        pxir_oracle::native_add(a, b, expected);
        const OwnedArray<float> c = add_f32(a, b);
        if (!PXIR_CHECK(c.size() == n && pxir_oracle::exactly_equal(c.view(), expected))) {
            std::fprintf(stderr, "  f32 n=%u\n", n);
        }
    }
}

void i32_edge_lengths() {
    for (const std::uint32_t n : edge_lengths) {
        std::mt19937_64 engine(500 + n);
        const auto a = pxir_oracle::generate_i32(engine, n);
        const auto b = pxir_oracle::generate_i32(engine, n);
        std::vector<std::int32_t> expected(n);
        pxir_oracle::native_add(a, b, expected);
        const OwnedArray<std::int32_t> c = add_i32(a, b);
        if (!PXIR_CHECK(c.size() == n && same_i32(c.view(), expected))) std::fprintf(stderr, "  i32 n=%u\n", n);
    }
}

void f32_special_values() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float denorm = std::numeric_limits<float>::denorm_min();
    constexpr float max = std::numeric_limits<float>::max();
    const std::vector<float> sa{0.0f, -0.0f, -0.0f, inf, max, denorm, nan, 1.0f, -inf};
    const std::vector<float> sb{-0.0f, -0.0f, 0.0f, -inf, max, denorm, 1.0f, 0x1p-24f, -inf};
    for (std::size_t offset : {std::size_t{0}, std::size_t{4}, std::size_t{8}}) {
        std::vector<float> a(17, 0.5f);
        std::vector<float> b(17, 0.25f);
        for (std::size_t k = 0; k < sa.size(); ++k) {
            a[(offset + k) % 17] = sa[k];
            b[(offset + k) % 17] = sb[k];
        }
        std::vector<float> expected(17);
        pxir_oracle::native_add(a, b, expected);
        PXIR_CHECK(pxir_oracle::exactly_equal(add_f32(a, b).view(), expected));
    }
    const std::vector<float> nz{-0.0f};
    PXIR_CHECK(std::signbit(add_f32(nz, nz)[0]));
}

void i32_wrap() {
    constexpr std::int32_t hi = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t lo = std::numeric_limits<std::int32_t>::min();
    const std::vector<std::int32_t> a{hi, lo, hi, lo, -1, 1, 0, hi};
    const std::vector<std::int32_t> b{1, -1, hi, lo, 1, -1, lo, lo};
    const std::vector<std::int32_t> expected{lo, hi, -2, 0, 0, 0, lo, -1};
    PXIR_CHECK(same_i32(add_i32(a, b).view(), expected));
}

// Copies are deep: same size and values, distinct storage, and writing one
// never changes the other.
template <class T>
void copy_is_deep(const OwnedArray<T>& original) {
    OwnedArray<T> source = original;  // mutable copy to probe independence
    const OwnedArray<T> copy = source;
    PXIR_CHECK(copy.size() == source.size());
    PXIR_CHECK(copy.data() != source.data());
    for (std::size_t i = 0; i < source.size(); ++i) PXIR_CHECK(copy[i] == source[i]);
    const T before = copy[0];
    source[0] = static_cast<T>(source[0] + T{1});
    PXIR_CHECK(copy[0] == before);

    OwnedArray<T> assigned = OwnedArray<T>::for_overwrite(1);
    assigned[0] = T{};
    assigned = copy;  // copy assignment
    PXIR_CHECK(assigned.size() == copy.size() && assigned.data() != copy.data());
    for (std::size_t i = 0; i < copy.size(); ++i) PXIR_CHECK(assigned[i] == copy[i]);

    auto& self = assigned;
    assigned = self;  // self-assignment keeps contents
    PXIR_CHECK(assigned.size() == copy.size());
    for (std::size_t i = 0; i < copy.size(); ++i) PXIR_CHECK(assigned[i] == copy[i]);
}

void copies_are_deep() {
    const std::vector<float> fa{1.5f, 2.5f, 3.5f, 4.5f, 5.5f};
    copy_is_deep(add_f32(fa, fa));
    const std::vector<std::int32_t> ia{1, 2, 3, 4, 5};
    copy_is_deep(add_i32(ia, ia));
}

void moves_transfer_ownership() {
    const std::vector<float> fa{1.0f, 2.0f, 3.0f};
    OwnedArray<float> a = add_f32(fa, fa);
    const float* storage = a.data();

    OwnedArray<float> b(std::move(a));  // move construction
    PXIR_CHECK(b.data() == storage && b.size() == 3);
    PXIR_CHECK(a.size() == 0 && a.data() == nullptr);  // NOLINT: moved-from state is specified here

    OwnedArray<float> c;
    c = std::move(b);  // move assignment
    PXIR_CHECK(c.data() == storage && c.size() == 3);
    PXIR_CHECK(b.size() == 0 && b.data() == nullptr);  // NOLINT

    // Growth of a vector of arrays moves elements; storage identity survives.
    std::vector<OwnedArray<float>> many;
    many.push_back(std::move(c));
    for (int i = 0; i < 64; ++i) many.push_back(add_f32(fa, fa));
    PXIR_CHECK(many[0].data() == storage);
    PXIR_CHECK(many[0][0] == 2.0f && many[0][2] == 6.0f);
}

}  // namespace

int main() {
    f32_edge_lengths();
    i32_edge_lengths();
    f32_special_values();
    i32_wrap();
    copies_are_deep();
    moves_transfer_ownership();
    return pxir_test::finish("owned_array");
}
