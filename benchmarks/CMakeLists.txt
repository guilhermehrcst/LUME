add_executable(pxir_bench_vector_add vector_add.cpp)
target_link_libraries(pxir_bench_vector_add PRIVATE pxir pxir_oracle)
target_compile_definitions(pxir_bench_vector_add PRIVATE "PXIR_BUILD_CONFIG=\"$<CONFIG>\"")
pxir_set_warnings(pxir_bench_vector_add)

if(BUILD_TESTING)
    # Smoke test only: proves the benchmark runs and its correctness check
    # passes. Timings from CI are not performance evidence.
    add_test(NAME pxir_bench_vector_add_smoke COMMAND pxir_bench_vector_add 4099 42 1 3)
    set_tests_properties(pxir_bench_vector_add_smoke PROPERTIES
        LABELS "pxir;benchmark"
        PASS_REGULAR_EXPRESSION "correctness=exact"
    )
endif()

# M1 executor cost breakdown. Includes the library's internal
# input_validation.hpp so it times the executor's real validation code.
add_executable(pxir_bench_executor_breakdown executor_breakdown.cpp)
target_link_libraries(pxir_bench_executor_breakdown PRIVATE pxir pxir_oracle)
target_include_directories(pxir_bench_executor_breakdown PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
target_compile_definitions(pxir_bench_executor_breakdown PRIVATE "PXIR_BUILD_CONFIG=\"$<CONFIG>\"")
pxir_set_warnings(pxir_bench_executor_breakdown)

if(BUILD_TESTING)
    add_test(NAME pxir_bench_executor_breakdown_smoke
        COMMAND pxir_bench_executor_breakdown sizes=1,4099 warmup=1 iterations=3)
    set_tests_properties(pxir_bench_executor_breakdown_smoke PROPERTIES
        LABELS "pxir;benchmark"
        PASS_REGULAR_EXPRESSION "n=4099 component=pxir_execution_total.*correctness=exact"
        FAIL_REGULAR_EXPRESSION "MISMATCH"
    )
endif()
