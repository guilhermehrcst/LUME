// M5 supplementary evidence (diagnostic, not part of the build). Build: g++|clang++ -std=c++20 -O3 -DNDEBUG dual_output_probe.cpp
// Diagnostic probe (not part of the build): fused vs two-pass vs dual output
// on identical arrays, plus a dual variant whose pointers are declared
// non-aliasing (__restrict, GCC/Clang extension) to test whether the runtime
// alias check / fallback path explains the dual kernel's mid-N cost.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <vector>
using F = float;
__attribute__((noinline)) void fused(const F* a, const F* b, const F* c, F* e, std::size_t n) { for (std::size_t i = 0; i < n; ++i) e[i] = (a[i] + b[i]) + c[i]; }
__attribute__((noinline)) void two_pass(const F* a, const F* b, const F* c, F* d, F* e, std::size_t n) { for (std::size_t i = 0; i < n; ++i) d[i] = a[i] + b[i]; for (std::size_t i = 0; i < n; ++i) e[i] = d[i] + c[i]; }
__attribute__((noinline)) void dual(const F* a, const F* b, const F* c, F* d, F* e, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { const F t = a[i] + b[i]; d[i] = t; e[i] = t + c[i]; } }
__attribute__((noinline)) void dual_restrict(const F* __restrict a, const F* __restrict b, const F* __restrict c, F* __restrict d, F* __restrict e, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { const F t = a[i] + b[i]; d[i] = t; e[i] = t + c[i]; } }
__attribute__((noinline)) void dual_e_first(const F* a, const F* b, const F* c, F* d, F* e, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { const F t = a[i] + b[i]; e[i] = t + c[i]; d[i] = t; } }
template <class K> double bench(K k) { std::vector<double> t; for (int r = 0; r < 3001; ++r) { auto t0 = std::chrono::steady_clock::now(); k(); t.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count()); } std::sort(t.begin(), t.end()); return t[t.size()/2]; }
int main() {
    for (std::size_t n : {4096u, 65536u, 1048576u}) {
        std::vector<F> a(n, 1), b(n, 2), c(n, 3), d(n), e(n);
        const double f = bench([&] { fused(a.data(), b.data(), c.data(), e.data(), n); });
        const double t = bench([&] { two_pass(a.data(), b.data(), c.data(), d.data(), e.data(), n); });
        const double du = bench([&] { dual(a.data(), b.data(), c.data(), d.data(), e.data(), n); });
        const double dr = bench([&] { dual_restrict(a.data(), b.data(), c.data(), d.data(), e.data(), n); });
        const double de = bench([&] { dual_e_first(a.data(), b.data(), c.data(), d.data(), e.data(), n); });
        std::printf("N=%7zu fused=%9.0f two_pass=%9.0f (%.2fx) dual=%9.0f (%.2fx) dual_restrict=%9.0f (%.2fx) dual_e_first=%9.0f (%.2fx) ns\n", n, f, t, t/f, du, du/f, dr, dr/f, de, de/f);
    }
}
