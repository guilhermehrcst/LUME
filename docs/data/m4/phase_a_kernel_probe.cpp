// M4 Phase A vectorization probe: the exact single-write indexed loops that
// Phase B would use, compiled standalone (not part of the build).
//   g++     -std=c++20 -O3 -DNDEBUG -Iinclude -c docs/data/m4/phase_a_kernel_probe.cpp -fopt-info-vec-all
//   clang++ -std=c++20 -O3 -DNDEBUG -Iinclude -c docs/data/m4/phase_a_kernel_probe.cpp -Rpass=loop-vectorize
#include <cstddef>
#include <cstdint>
#include <span>

#include "pxir/runtime/owned_array.hpp"

namespace {

std::int32_t wrapping_add(std::int32_t a, std::int32_t b) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

}  // namespace

pxir::OwnedArray<float> m4_add_f32(std::span<const float> a, std::span<const float> b) {
    auto c = pxir::OwnedArray<float>::for_overwrite(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) c[i] = a[i] + b[i];
    return c;
}

pxir::OwnedArray<std::int32_t> m4_add_i32(std::span<const std::int32_t> a, std::span<const std::int32_t> b) {
    auto c = pxir::OwnedArray<std::int32_t>::for_overwrite(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) c[i] = wrapping_add(a[i], b[i]);
    return c;
}
