#pragma once

// Minimal assertion support for Lume tests; no external framework.

#include <cstdio>

namespace lume_test {

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline bool check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        ++failure_count();
        std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
    return ok;
}

inline int finish(const char* suite) {
    const int failures = failure_count();
    std::printf("%s: %s (%d failure%s)\n", suite, failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

}  // namespace lume_test

#define LUME_CHECK(expr) ::lume_test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
