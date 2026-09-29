#pragma once

#include <algorithm>
#include <cstdio>

#include "pxir/verify/verifier.hpp"

namespace pxir_test {

inline bool has_code(const pxir::VerifyResult& result, pxir::DiagnosticCode code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [code](const pxir::Diagnostic& d) { return d.code == code; });
}

// A rejection must carry at least one diagnostic and no verified program.
inline bool rejected_with(const pxir::VerifyResult& result, pxir::DiagnosticCode code) {
    const bool ok = !result.ok() && has_code(result, code);
    if (!ok) {
        for (const auto& d : result.diagnostics) std::fprintf(stderr, "  diagnostic: %s\n", d.message.c_str());
    }
    return ok;
}

}  // namespace pxir_test
