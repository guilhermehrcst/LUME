// M6 supplementary microbenchmark (not part of the build; the primary A/B is
// the M5 executor benchmark). It isolates the second pass's destination from
// every allocation effect: all arrays are preallocated and pre-touched.
//   out_of_place: D = A + B ; E = D + C   (24 B/element user-level payload)
//   in_place:     D = A + B ; D = D + C   (24 B/element user-level payload)
//   fused:        E = (A + B) + C         (16 B/element, reference only)
// Same steady-state harness shape as M1/M5: 5 warmup, 51 timed iterations,
// median; K = 1000 repetitions per interval below N = 4096.
// Build: g++|clang++ -std=c++20 -O3 -DNDEBUG docs/data/m6/inplace_microbench.cpp
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

__attribute__((noinline)) void add_out(const float* a, const float* b, float* c, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) c[i] = a[i] + b[i];
}
__attribute__((noinline)) void add_in(float* d, const float* c, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) d[i] = d[i] + c[i];
}
__attribute__((noinline)) void fused(const float* a, const float* b, const float* c, float* e, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) e[i] = (a[i] + b[i]) + c[i];
}

template <class F>
double median_ns(F&& f, int k) {
    std::vector<double> s;
    for (int it = 0; it < 56; ++it) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < k; ++r) f();
        const auto t1 = std::chrono::steady_clock::now();
        if (it >= 5) s.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / k);
    }
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

}  // namespace

int main() {
    for (const std::size_t n : {4096ul, 65536ul, 1048576ul, 4194304ul}) {
        auto a = std::make_unique<float[]>(n);  // value-initialized: pages touched
        auto b = std::make_unique<float[]>(n);
        auto c = std::make_unique<float[]>(n);
        auto d = std::make_unique<float[]>(n);
        auto e = std::make_unique<float[]>(n);
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = static_cast<float>(i % 97) * 0.25f;
            b[i] = static_cast<float>(i % 89) * 0.5f;
            c[i] = static_cast<float>(i % 83);
        }
        const int k = n < 4096 ? 1000 : 1;
        const double oop = median_ns([&] { add_out(a.get(), b.get(), d.get(), n); add_out(d.get(), c.get(), e.get(), n); }, k);
        const double inp = median_ns([&] { add_out(a.get(), b.get(), d.get(), n); add_in(d.get(), c.get(), n); }, k);
        const double fus = median_ns([&] { fused(a.get(), b.get(), c.get(), e.get(), n); }, k);
        std::printf("n=%zu out_of_place_ns=%.0f in_place_ns=%.0f fused_ns=%.0f in_place/out_of_place=%.3f\n", n, oop, inp, fus,
                    inp / oop);
    }
    return 0;
}
