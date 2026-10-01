add_executable(lume_bench_vector_add vector_add.cpp)
target_link_libraries(lume_bench_vector_add PRIVATE lume lume_oracle)
target_compile_definitions(lume_bench_vector_add PRIVATE "LUME_BUILD_CONFIG=\"$<CONFIG>\"")
lume_set_warnings(lume_bench_vector_add)

if(BUILD_TESTING)
    # Smoke test only: proves the benchmark runs and its correctness check
    # passes. Timings from CI are not performance evidence.
    add_test(NAME lume_bench_vector_add_smoke COMMAND lume_bench_vector_add 4099 42 1 3)
    set_tests_properties(lume_bench_vector_add_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "correctness=exact"
    )
endif()

# M1 executor cost breakdown. Includes the library's internal
# input_validation.hpp so it times the executor's real validation code.
add_executable(lume_bench_executor_breakdown executor_breakdown.cpp)
target_link_libraries(lume_bench_executor_breakdown PRIVATE lume lume_oracle)
target_include_directories(lume_bench_executor_breakdown PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
target_compile_definitions(lume_bench_executor_breakdown PRIVATE "LUME_BUILD_CONFIG=\"$<CONFIG>\"")
lume_set_warnings(lume_bench_executor_breakdown)

if(BUILD_TESTING)
    add_test(NAME lume_bench_executor_breakdown_smoke
        COMMAND lume_bench_executor_breakdown sizes=1,4099 warmup=1 iterations=3)
    set_tests_properties(lume_bench_executor_breakdown_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "n=4099 component=lume_execution_total.*correctness=exact"
        FAIL_REGULAR_EXPRESSION "MISMATCH"
    )
endif()

# M5 intermediate materialization baseline (D = A + B; E = D + C). Includes
# the library's internal input_validation.hpp for the chain's real validation.
add_executable(lume_bench_intermediate_materialization intermediate_materialization.cpp)
target_link_libraries(lume_bench_intermediate_materialization PRIVATE lume lume_oracle)
target_include_directories(lume_bench_intermediate_materialization PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
target_compile_definitions(lume_bench_intermediate_materialization PRIVATE "LUME_BUILD_CONFIG=\"$<CONFIG>\"")
lume_set_warnings(lume_bench_intermediate_materialization)

if(BUILD_TESTING)
    add_test(NAME lume_bench_intermediate_materialization_smoke
        COMMAND lume_bench_intermediate_materialization sizes=1,4099 warmup=1 iterations=3)
    set_tests_properties(lume_bench_intermediate_materialization_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "n=4099 component=lume_chain_output_after_use.*correctness=exact"
        FAIL_REGULAR_EXPRESSION "MISMATCH"
    )
endif()
