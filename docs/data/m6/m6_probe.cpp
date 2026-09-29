// M6 probe (not part of the build). Runs a PXIR program `iters` times at
// N = 1,048,576 (or argv[3]); each ExecutionResult is destroyed at the end of
// its iteration. Used for strace and for the supplementary longer-chain A/B,
// built against both the canonical M5 tree and the M6 tree.
//   mode "final":        D = A + B; E = D + C; output E
//   mode "intermediate": D = A + B; output D; E = D + C; output E
//   mode "after_use":    D = A + B; E = D + C; output D; output E
//   mode "chain4":       V1 = A + B; V2 = V1 + C; V3 = V2 + G; V4 = V3 + H; output V4
// Build: g++ -std=c++20 -O3 -DNDEBUG -Iinclude -Ioracle/include docs/data/m6/m6_probe.cpp <build>/src/libpxir.a
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <sys/resource.h>

#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"
#include "pxir_oracle/oracle.hpp"

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "final";
    const int iters = argc > 2 ? std::atoi(argv[2]) : 10;
    const std::uint32_t n = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 1048576;
    std::mt19937_64 e(42);
    const bool chain4 = std::strcmp(mode, "chain4") == 0;
    const int input_count = chain4 ? 5 : 3;  // same inputs as the M5 strace probe for the 3-input shapes
    std::vector<pxir::Buffer> in;
    for (int k = 0; k < input_count; ++k) in.emplace_back(pxir_oracle::generate_f32(e, n));
    pxir::Program p;
    pxir::ValueId v[5];
    for (int k = 0; k < input_count; ++k) v[k] = p.input(pxir::f32, n);
    if (chain4) {
        auto x = p.add(v[0], v[1]);
        x = p.add(x, v[2]);
        x = p.add(x, v[3]);
        x = p.add(x, v[4]);
        p.output(x);
    } else {
        const auto d = p.add(v[0], v[1]);
        if (std::strcmp(mode, "intermediate") == 0) p.output(d);
        const auto ee = p.add(d, v[2]);
        if (std::strcmp(mode, "after_use") == 0) p.output(d);
        p.output(ee);
    }
    auto ver = pxir::verify(std::move(p));
    if (!ver.ok()) return 1;
    const std::vector<pxir::Buffer>& inputs = in;
    rusage r0{};
    getrusage(RUSAGE_SELF, &r0);
    std::vector<double> ns;
    std::fprintf(stderr, "--- begin loop\n");
    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = pxir::execute_cpu_reference(*ver.program, inputs);
        const auto t1 = std::chrono::steady_clock::now();
        if (!r.ok()) return 1;
        ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    std::fprintf(stderr, "--- end loop\n");
    rusage r1{};
    getrusage(RUSAGE_SELF, &r1);
    std::sort(ns.begin(), ns.end());
    std::fprintf(stderr, "mode=%s n=%u iters=%d minor_faults_in_loop=%ld per_call=%.1f median_ns=%.0f\n", mode, n, iters,
                 r1.ru_minflt - r0.ru_minflt, static_cast<double>(r1.ru_minflt - r0.ru_minflt) / iters,
                 ns[ns.size() / 2]);
    return 0;
}
