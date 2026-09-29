// M5 supplementary evidence (diagnostic, not part of the build). Build: g++ -std=c++20 -O3 -DNDEBUG dual_output_offset_probe.cpp
// Diagnostic probe (not part of the build): is native_fused_dual_output's
// mid-N slowdown caused by address relationships between d_out and c (4K
// aliasing)? Times the same dual kernel with d_out at different offsets.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>
#include <algorithm>
__attribute__((noinline)) void dual(std::span<const float> a, std::span<const float> b, std::span<const float> c,
                                    std::span<float> d_out, std::span<float> e) {
    for (std::size_t i = 0; i < e.size(); ++i) { const float d = a[i] + b[i]; d_out[i] = d; e[i] = d + c[i]; }
}
int main() {
    const std::size_t n = 4096;
    std::vector<float> a(n, 1.0f), b(n, 2.0f), c(n, 3.0f), e(n);
    std::vector<float> pool(n + 4096);
    auto mod = [](const void* p) { return static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(p) % 4096); };
    std::printf("a%%4096=%u b=%u c=%u e=%u\n", mod(a.data()), mod(b.data()), mod(c.data()), mod(e.data()));
    for (std::size_t shift : {0, 1, 4, 8, 16, 64, 128, 256, 512, 1024}) {
        float* dp = pool.data() + shift;
        std::vector<double> t;
        for (int r = 0; r < 2001; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            dual(a, b, c, std::span<float>(dp, n), e);
            t.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(t.begin(), t.end());
        long long delta = (long long)(reinterpret_cast<std::uintptr_t>(dp) - reinterpret_cast<std::uintptr_t>(c.data()));
        std::printf("d_out shift=%4zu floats: (d_out - c) mod 4096 = %4lld B, (d_out - e) mod 4096 = %4lld B -> median %.0f ns\n",
                    shift, ((delta % 4096) + 4096) % 4096,
                    (((long long)(reinterpret_cast<std::uintptr_t>(dp) - reinterpret_cast<std::uintptr_t>(e.data()))) % 4096 + 4096) % 4096,
                    t[t.size() / 2]);
    }
}
