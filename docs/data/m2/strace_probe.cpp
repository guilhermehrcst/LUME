// Runs the executor `iters` times at N=1M, freeing each result after the call (as the benchmarks do).
#include <cstdio>
#include <cstdlib>
#include <random>
#include "pxir/runtime/cpu_reference.hpp"
#include "pxir_oracle/oracle.hpp"
int main(int argc, char** argv) {
  const int iters = argc > 1 ? std::atoi(argv[1]) : 10;
  const std::uint32_t n = 1048576;
  std::mt19937_64 e(42);
  std::vector<pxir::Buffer> in{pxir::Buffer(pxir_oracle::generate_f32(e, n)), pxir::Buffer(pxir_oracle::generate_f32(e, n))};
  pxir::Program p; p.output(p.add(p.input(pxir::f32, n), p.input(pxir::f32, n)));
  auto v = pxir::verify(std::move(p));
  std::fprintf(stderr, "--- begin loop\n");
  for (int i = 0; i < iters; ++i) { auto r = pxir::execute_cpu_reference(*v.program, in); if (!r.ok()) return 1; }
  std::fprintf(stderr, "--- end loop\n");
}
