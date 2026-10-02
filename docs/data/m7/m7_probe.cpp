// M7 probe (not part of the build). Runs a Lume program `iters` times at
// N = 1,048,576 (or argv[3]); each ExecutionResult is destroyed at the end of
// its iteration. Used for strace and per-call fault counts, built against both
// canonical main (M6 runtime) and the M7 tree.
//   mode "final":        D = A + B; E = D + C; output E        (M7 fuses)
//   mode "right":        D = A + B; E = C + D; output E        (M7 fuses)
//   mode "intermediate": D = A + B; output D; E = D + C; output E
//   mode "after_use":    D = A + B; E = D + C; output D; output E
//   mode "single":       D = A + B; output D
//   mode "chain3":       V1 = A + B; V2 = V1 + C; V3 = V2 + F; output V3
//   mode "chain4":       ... V4 = V3 + G; output V4
// For chain3/chain4 the probe also times a native single loop computing the
// same left-to-right sums into a preallocated, pre-touched buffer (reference).
// Build: g++ -std=c++20 -O3 -DNDEBUG -Iinclude -Ioracle/include docs/data/m7/m7_probe.cpp <build>/src/liblume.a
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <sys/resource.h>

#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"
#include "lume_oracle/oracle.hpp"

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "final";
    const int iters = argc > 2 ? std::atoi(argv[2]) : 10;
    const std::uint32_t n = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 1048576;
    const int depth = std::strcmp(mode, "chain3") == 0 ? 3 : std::strcmp(mode, "chain4") == 0 ? 4 : 0;
    std::mt19937_64 e(42);
    std::vector<std::vector<float>> raw;
    for (int k = 0; k < (depth > 0 ? depth + 1 : 3); ++k) raw.push_back(lume_oracle::generate_f32(e, n));
    std::vector<lume::Buffer> in;
    for (const auto& r : raw) in.emplace_back(r);
    lume::Program p;
    std::vector<lume::ValueId> v;
    for (std::size_t k = 0; k < raw.size(); ++k) v.push_back(p.input(lume::f32, n));
    const auto a = v[0];
    const auto b = v[1];
    const auto c = v[2];
    const auto d = p.add(a, b);
    if (depth > 0) {
        auto x = d;
        for (int k = 2; k <= depth; ++k) x = p.add(x, v[static_cast<std::size_t>(k)]);
        p.output(x);
    } else if (std::strcmp(mode, "single") == 0) {
        p.output(d);
    } else {
        if (std::strcmp(mode, "intermediate") == 0) p.output(d);
        const auto ee = std::strcmp(mode, "right") == 0 ? p.add(c, d) : p.add(d, c);
        if (std::strcmp(mode, "after_use") == 0) p.output(d);
        p.output(ee);
    }
    auto ver = lume::verify(std::move(p));
    if (!ver.ok()) return 1;
    std::vector<double> ns;
    ns.reserve(static_cast<std::size_t>(iters));
    rusage r0{};
    getrusage(RUSAGE_SELF, &r0);
    std::fprintf(stderr, "--- begin loop\n");
    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = lume::execute_cpu_reference(*ver.program, in);
        const auto t1 = std::chrono::steady_clock::now();
        if (!r.ok()) return 1;
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    std::fprintf(stderr, "--- end loop\n");
    rusage r1{};
    getrusage(RUSAGE_SELF, &r1);
    std::sort(ns.begin(), ns.end());
    double native_ns = 0;
    if (depth > 0) {
        // Native reference: one loop, same left-to-right grouping, preallocated output.
        std::vector<float> out(n, 0.0f);
        std::vector<double> t;
        for (int i = 0; i < iters + 5; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const float* r0 = raw[0].data();
            const float* r1 = raw[1].data();
            const float* r2 = raw[2].data();
            const float* r3 = raw[3].data();
            float* o = out.data();
            if (depth == 3) {
                for (std::uint32_t j = 0; j < n; ++j) o[j] = ((r0[j] + r1[j]) + r2[j]) + r3[j];
            } else {
                const float* r4 = raw[4].data();
                for (std::uint32_t j = 0; j < n; ++j) o[j] = (((r0[j] + r1[j]) + r2[j]) + r3[j]) + r4[j];
            }
            const auto t1 = std::chrono::steady_clock::now();
            if (i >= 5) t.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        std::sort(t.begin(), t.end());
        native_ns = t[t.size() / 2];
        if (out[n / 2] != out[n / 2]) return 1;  // keep the reference loop observable
    }
    std::fprintf(stderr, "mode=%s n=%u iters=%d minor_faults_in_loop=%ld per_call=%.1f median_ns=%.0f native_single_loop_ns=%.0f\n",
                 mode, n, iters, r1.ru_minflt - r0.ru_minflt,
                 static_cast<double>(r1.ru_minflt - r0.ru_minflt) / iters, ns[ns.size() / 2], native_ns);
    return 0;
}
