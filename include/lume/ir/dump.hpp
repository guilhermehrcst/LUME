#pragma once

#include <string>

#include "lume/ir/program.hpp"

namespace lume {

// Human-readable dump for debugging, one operation per line:
//
//   %0 = input f32[1024]
//   %1 = input f32[1024]
//   %2 = add %0, %1
//   output %2
//
// This is NOT a Lume textual language and there is no parser for it. It is
// safe to call on unverified or malformed programs.
[[nodiscard]] std::string to_debug_string(const Program& program);

}  // namespace lume
