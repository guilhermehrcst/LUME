// M5 strace probe (not part of the build): runs a PXIR chain `iters` times at
// N = 1,048,576; each ExecutionResult is destroyed at the end of its iteration.
//   mode "final":        D = A + B; E = D + C; output E
//   mode "intermediate": D = A + B; output D; E = D + C; output E
// Build: g++ -std=c++20 -O2 -Iinclude -Ioracle/include docs/data/m5/strace_probe_chain.cpp build/src/libpxir.a
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
    const bool intermediate = argc > 1 && std::strcmp(argv[1], "intermediate") == 0;
    const int iters = argc > 2 ? std::atoi(argv[2]) : 10;
    const std::uint32_t n = 1048576;
    std::mt19937_64 e(42);
    std::vector<pxir::Buffer> in{pxir::Buffer(pxir_oracle::generate_f32(e, n)),
                                 pxir::Buffer(pxir_oracle::generate_f32(e, n)),
                                 pxir::Buffer(pxir_oracle::generate_f32(e, n))};
    pxir::Program p;
    const auto a = p.input(pxir::f32, n);
    const auto b = p.input(pxir::f32, n);
    const auto c = p.input(pxir::f32, n);
    const auto d = p.add(a, b);
    if (intermediate) p.output(d);
    p.output(p.add(d, c));
    auto v = pxir::verify(std::move(p));
    if (!v.ok()) return 1;
    rusage r0{};
    getrusage(RUSAGE_SELF, &r0);
    std::fprintf(stderr, "--- begin loop\n");
    for (int i = 0; i < iters; ++i) {
        auto r = pxir::execute_cpu_reference(*v.program, in);
        if (!r.ok()) return 1;
    }
    std::fprintf(stderr, "--- end loop\n");
    rusage r1{};
    getrusage(RUSAGE_SELF, &r1);
    std::fprintf(stderr, "minor_faults_in_loop=%ld per_call=%.1f\n", r1.ru_minflt - r0.ru_minflt,
                 static_cast<double>(r1.ru_minflt - r0.ru_minflt) / iters);
    return 0;
}
