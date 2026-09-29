// PXIR M0 baseline benchmark: C = A + B over f32[N].
//
// Compares a native scalar C++ loop (the oracle) against PXIR construction,
// verification, and scalar reference execution. It establishes a baseline;
// it makes no performance claim. Output is one key=value per line.
//
// usage: pxir_bench_vector_add [elements] [seed] [warmup] [iterations]

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "pxir/ir/dump.hpp"
#include "pxir/ir/program.hpp"
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir/verify/verifier.hpp"
#include "pxir_oracle/oracle.hpp"

#ifndef PXIR_BUILD_CONFIG
#define PXIR_BUILD_CONFIG "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::uint32_t elements = 1u << 20;
    std::uint64_t seed = 42;
    std::uint32_t warmup = 5;
    std::uint32_t iterations = 51;
};

// Digits only, no sign, no overflow, within [min, max].
std::optional<std::uint64_t> parse_uint(const char* text, std::uint64_t min, std::uint64_t max) {
    if (text == nullptr || *text == '\0') return std::nullopt;
    std::uint64_t value = 0;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(*c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    if (value < min || value > max) return std::nullopt;
    return value;
}

struct Stats {
    std::int64_t median = 0;
    std::int64_t min = 0;
    std::int64_t max = 0;
};

Stats summarize(std::vector<std::int64_t> ns) {
    std::sort(ns.begin(), ns.end());
    return Stats{ns[ns.size() / 2], ns.front(), ns.back()};
}

std::int64_t elapsed_ns(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

void print_stats(const char* name, const Stats& s) {
    std::printf("%s_ns_median=%" PRId64 "\n%s_ns_min=%" PRId64 "\n%s_ns_max=%" PRId64 "\n", name, s.median, name, s.min,
                name, s.max);
}

std::string compiler_id() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

pxir::Program build_program(std::uint32_t n) {
    pxir::Program program;
    const pxir::ValueId a = program.input(pxir::f32, n);
    const pxir::ValueId b = program.input(pxir::f32, n);
    program.output(program.add(a, b));
    return program;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (argc > 5) {
        std::fprintf(stderr, "usage: %s [elements] [seed] [warmup] [iterations]\n", argv[0]);
        return 2;
    }
    const std::uint64_t max_elements = std::numeric_limits<std::uint32_t>::max() - 1;
    const auto parse_arg = [&](int index, std::uint64_t min, std::uint64_t max) -> std::optional<std::uint64_t> {
        if (argc <= index) return std::nullopt;
        const auto v = parse_uint(argv[index], min, max);
        if (!v) {
            std::fprintf(stderr, "error: argument %d '%s' must be an integer in [%" PRIu64 ", %" PRIu64 "]\n", index,
                         argv[index], min, max);
            std::exit(2);
        }
        return v;
    };
    if (auto v = parse_arg(1, 1, max_elements)) opt.elements = static_cast<std::uint32_t>(*v);
    if (auto v = parse_arg(2, 0, std::numeric_limits<std::uint64_t>::max())) opt.seed = *v;
    if (auto v = parse_arg(3, 0, 1'000'000)) opt.warmup = static_cast<std::uint32_t>(*v);
    if (auto v = parse_arg(4, 1, 1'000'000)) opt.iterations = static_cast<std::uint32_t>(*v);

    const std::size_t n = opt.elements;

    // Identical inputs for both paths: A then B from one seeded stream.
    std::mt19937_64 engine(opt.seed);
    const std::vector<float> a = pxir_oracle::generate_f32(engine, n);
    const std::vector<float> b = pxir_oracle::generate_f32(engine, n);
    const std::vector<pxir::Buffer> inputs{pxir::Buffer(a), pxir::Buffer(b)};

    // ---- PXIR construction and verification --------------------------------
    std::vector<std::int64_t> construction_ns;
    std::vector<std::int64_t> verification_ns;
    std::optional<pxir::VerifiedProgram> verified;
    for (std::uint32_t i = 0; i < opt.warmup + opt.iterations; ++i) {
        auto start = Clock::now();
        pxir::Program program = build_program(opt.elements);
        const std::int64_t built = elapsed_ns(start);

        start = Clock::now();
        pxir::VerifyResult result = pxir::verify(std::move(program));
        const std::int64_t checked = elapsed_ns(start);

        if (!result.ok()) {
            for (const auto& d : result.diagnostics) std::fprintf(stderr, "%s\n", d.message.c_str());
            std::printf("correctness=verification_failed\n");
            return 1;
        }
        if (i >= opt.warmup) {
            construction_ns.push_back(built);
            verification_ns.push_back(checked);
        }
        verified = std::move(result.program);
    }

    // ---- Native baseline ----------------------------------------------------
    std::vector<float> native_c(n);
    std::vector<std::int64_t> native_ns;
    for (std::uint32_t i = 0; i < opt.warmup + opt.iterations; ++i) {
        const auto start = Clock::now();
        pxir_oracle::native_add(a, b, native_c);
        const std::int64_t t = elapsed_ns(start);
        if (i >= opt.warmup) native_ns.push_back(t);
    }

    // ---- PXIR reference execution -------------------------------------------
    // Timed region: input validation, interpretation, result allocation, add
    // loop, and output copy.
    std::vector<std::int64_t> pxir_ns;
    bool all_equal = true;
    std::uint64_t pxir_checksum = 0;
    for (std::uint32_t i = 0; i < opt.warmup + opt.iterations; ++i) {
        const auto start = Clock::now();
        pxir::ExecutionResult result = pxir::execute_cpu_reference(*verified, inputs);
        const std::int64_t t = elapsed_ns(start);
        if (!result.ok()) {
            std::fprintf(stderr, "execution error: %s\n", result.error->message.c_str());
            std::printf("correctness=execution_failed\n");
            return 1;
        }
        // Every run, warmup included, is checked against the oracle outside the timed region.
        const std::span<const float> c = *result.outputs.at(0).f32_view();
        all_equal = all_equal && pxir_oracle::exactly_equal(c, native_c);
        pxir_checksum = pxir_oracle::fnv1a(c);
        if (i >= opt.warmup) pxir_ns.push_back(t);
    }

    const pxir::Program& program = verified->program();
    const pxir::ProgramStorage& storage = program.storage();
    const pxir::StorageFootprint footprint = pxir::storage_footprint(program);

    std::printf("pxir_benchmark=vector_add\n");
    std::printf("workload=C=A+B\n");
    std::printf("dtype=f32\n");
    std::printf("elements=%zu\n", n);
    std::printf("seed=%" PRIu64 "\n", opt.seed);
    std::printf("input_generator=mt19937_64_24bit_uniform_[-1,1)\n");
    std::printf("warmup=%u\n", static_cast<unsigned>(opt.warmup));
    std::printf("iterations=%u\n", static_cast<unsigned>(opt.iterations));
    std::printf("statistic=median_of_iterations\n");
    std::printf("compiler=%s\n", compiler_id().c_str());
    std::printf("build_config=%s\n", PXIR_BUILD_CONFIG);

    std::istringstream dump(pxir::to_debug_string(program));
    for (std::string line; std::getline(dump, line);) std::printf("ir=%s\n", line.c_str());
    std::printf("ir_operations=%zu\n", storage.operations.size());
    std::printf("ir_values=%zu\n", storage.values.size());
    std::printf("ir_types=%zu\n", storage.types.size());
    std::printf("sizeof_ValueId=%zu\n", sizeof(pxir::ValueId));
    std::printf("sizeof_TypeId=%zu\n", sizeof(pxir::TypeId));
    std::printf("sizeof_OperationId=%zu\n", sizeof(pxir::OperationId));
    std::printf("sizeof_Type=%zu\n", sizeof(pxir::Type));
    std::printf("sizeof_Value=%zu\n", sizeof(pxir::Value));
    std::printf("sizeof_Operation=%zu\n", sizeof(pxir::Operation));
    std::printf("sizeof_Program=%zu\n", sizeof(pxir::Program));
    std::printf("ir_storage_used_bytes=%zu\n", footprint.used_bytes);
    std::printf("ir_storage_reserved_bytes=%zu\n", footprint.reserved_bytes);

    print_stats("construction", summarize(construction_ns));
    print_stats("verification", summarize(verification_ns));
    print_stats("native", summarize(native_ns));
    print_stats("pxir_execution", summarize(pxir_ns));

    std::printf("native_checksum_fnv1a=0x%016" PRIx64 "\n", pxir_oracle::fnv1a(native_c));
    std::printf("pxir_checksum_fnv1a=0x%016" PRIx64 "\n", pxir_checksum);
    std::printf("correctness=%s\n", all_equal ? "exact" : "MISMATCH");
    return all_equal ? 0 : 1;
}
