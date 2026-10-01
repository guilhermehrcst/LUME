#pragma once

#include <algorithm>
#include <cstdio>

#include "lume/verify/verifier.hpp"

namespace lume_test {

inline bool has_code(const lume::VerifyResult& result, lume::DiagnosticCode code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [code](const lume::Diagnostic& d) { return d.code == code; });
}

// A rejection must carry at least one diagnostic and no verified program.
inline bool rejected_with(const lume::VerifyResult& result, lume::DiagnosticCode code) {
    const bool ok = !result.ok() && has_code(result, code);
    if (!ok) {
        for (const auto& d : result.diagnostics) std::fprintf(stderr, "  diagnostic: %s\n", d.message.c_str());
    }
    return ok;
}

}  // namespace lume_test
