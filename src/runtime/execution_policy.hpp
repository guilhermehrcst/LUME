#pragma once

// Internal to the lume library (not installed under include/). Experiment E1
// needs to run the same verified program under the three execution plans that
// the milestones introduced one after another (P5: materialize every result,
// P6: + last-use reuse, P7: + strict fusion), and to read what the executor
// actually did. The default policy is exactly execute_cpu_reference.

#include <cstdint>
#include <span>
#include <vector>

#include "lume/runtime/buffer.hpp"
#include "lume/runtime/cpu_reference.hpp"
#include "lume/verify/verifier.hpp"

namespace lume::detail {

struct ExecutionPolicy {
    bool reuse = true;  // M6: compute an add inside an owned operand at its last use
    bool fuse = true;   // M7: run a strictly eligible adjacent add pair as one loop
    // Experiment E2 only (default off, so default behaviour is unchanged):
    // never store a shared intermediate D = A + B whose operands are caller
    // inputs and whose every use is as exactly one operand of an add; each
    // consumer instead computes (A + B) + C in one loop into its own fresh
    // buffer. Deletable together with the experiment.
    bool recompute = false;
};

// What one execution did, counted by the executor itself.
struct ExecutionStats {
    std::uint32_t fresh_results = 0;     // add results written into a newly allocated buffer (a fused pair counts once)
    std::uint32_t in_place_results = 0;  // add results written into an operand's storage
    std::uint32_t fused_pairs = 0;       // add pairs executed as one loop (their first result is never stored)
    std::uint32_t output_moves = 0;      // outputs transferred without copying
    std::uint32_t output_copies = 0;     // outputs deep-copied
    std::uint32_t recomputed_consumers = 0;  // E2: consumer adds that recomputed their elided producer
    std::uint32_t elided_producers = 0;      // E2: producer adds never executed on their own

    friend bool operator==(const ExecutionStats&, const ExecutionStats&) = default;
};

// Same as execute_cpu_reference, under `policy`. `stats` (optional) receives
// the counts of this call; `fused` (optional) the first operation index of
// every fused pair.
[[nodiscard]] ExecutionResult execute_cpu_reference_with_policy(const VerifiedProgram& program,
                                                                std::span<const Buffer> inputs,
                                                                const ExecutionPolicy& policy,
                                                                ExecutionStats* stats = nullptr,
                                                                std::vector<std::uint32_t>* fused = nullptr);

}  // namespace lume::detail
