// Self-contained loops with the same shape as the executor kernels (fresh add, fused add-add), with no
// standard headers, so they can be compiled for another architecture on a host that lacks that target's
// C++ library. Used ONLY to test codegen_report.py's disassembly parser; it is not the E2 code.
extern "C" {
void fixture_add_f32(const float* a, const float* b, float* c, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) c[i] = a[i] + b[i];
}
void fixture_add_add_f32(const float* a, const float* b, const float* c, float* r, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) r[i] = (a[i] + b[i]) + c[i];
}
void fixture_add_add_i32(const int* a, const int* b, const int* c, int* r, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) r[i] = (int)((unsigned)((unsigned)a[i] + (unsigned)b[i]) + (unsigned)c[i]);
}
}
