// M7 NaN-payload audit (not part of the build). Runs the fusible shapes
// D = A + B; E = D + C and D = A + B; E = C + D through whichever Lume library
// it is linked with and writes the raw output bits to a file. Built against
// canonical main (M6 runtime: two passes, second in place) and against M7
// (one fused loop), the two files are compared bit for bit: non-NaN elements
// must be identical; NaN payloads may differ (IEEE-754 leaves them open and the
// oracle treats any NaN as matching any NaN).
// Build: g++|clang++ -std=c++20 -O3 -DNDEBUG -Iinclude -Ioracle/include docs/data/m7/nan_audit.cpp <build>/src/liblume.a
// Usage: nan_audit <output-file>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    constexpr std::uint32_t n = 65536;
    std::mt19937_64 engine(2026);
    // Random finite values, with ~1/8 of elements replaced by NaNs of random
    // sign and payload (quiet and signaling) and ~1/32 by infinities.
    const auto make = [&] {
        std::vector<float> v(n);
        for (float& x : v) {
            const std::uint64_t r = engine();
            const auto kind = r & 31u;
            if (kind < 4) {
                const auto payload = static_cast<std::uint32_t>((r >> 8) & 0x7fffffu) | 1u;
                x = std::bit_cast<float>(((r >> 40) & 1u ? 0xff800000u : 0x7f800000u) | payload);
            } else if (kind == 4) {
                x = (r >> 40) & 1u ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            } else {
                x = static_cast<float>(static_cast<std::int32_t>(r >> 32)) * 0x1p-20f;
            }
        }
        return v;
    };
    const std::vector<lume::Buffer> in{lume::Buffer(make()), lume::Buffer(make()), lume::Buffer(make())};
    std::FILE* f = std::fopen(argv[1], "wb");
    if (f == nullptr) return 1;
    for (const bool right : {false, true}) {
        lume::Program p;
        const auto a = p.input(lume::f32, n);
        const auto b = p.input(lume::f32, n);
        const auto c = p.input(lume::f32, n);
        const auto d = p.add(a, b);
        p.output(right ? p.add(c, d) : p.add(d, c));
        auto v = lume::verify(std::move(p));
        if (!v.ok()) return 1;
        const auto r = lume::execute_cpu_reference(*v.program, in);
        if (!r.ok()) return 1;
        const auto out = *r.outputs[0].f32_view();
        std::fwrite(out.data(), sizeof(float), out.size(), f);
    }
    std::fclose(f);
    return 0;
}
