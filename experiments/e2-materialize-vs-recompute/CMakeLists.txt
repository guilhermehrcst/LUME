# Experiment E2: materialize vs recompute. Linux only (reuses the E1 workspace
# and allocation hooks). Deletable together with the experiment.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "E2 (materialize vs recompute) is Linux-only; skipped on ${CMAKE_SYSTEM_NAME}")
    return()
endif()

execute_process(
    COMMAND git -C ${PROJECT_SOURCE_DIR} rev-parse --short=12 HEAD
    OUTPUT_VARIABLE LUME_E2_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)
if(NOT LUME_E2_GIT_SHA)
    set(LUME_E2_GIT_SHA "unknown")
endif()
add_executable(lume_e2_bench e2_bench.cpp)
target_include_directories(lume_e2_bench PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(lume_e2_bench PRIVATE lume lume_oracle lume_e1_hooks)
target_compile_definitions(lume_e2_bench PRIVATE
    "LUME_E2_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
    "LUME_E2_FLAGS=\"${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE} (build type ${CMAKE_BUILD_TYPE})\""
    "LUME_E2_GIT_SHA=\"${LUME_E2_GIT_SHA}\""
)
lume_set_warnings(lume_e2_bench)

if(BUILD_TESTING)
    # Smoke test only: every k, both dtypes, all three regimes, both strategies
    # checked against the oracle. CI timings are not performance evidence.
    add_test(NAME lume_e2_bench_smoke
        COMMAND lume_e2_bench out=${CMAKE_CURRENT_BINARY_DIR}/smoke_raw.csv plans=${CMAKE_CURRENT_BINARY_DIR}/smoke_plans.csv
                replicate=smoke regimes=arena_cold,arena_warm,glibc_default_fresh ks=1,2,3,4,6,8 sizes=64,4099 reps=2 warmup=1)
    set_tests_properties(lume_e2_bench_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "done i32 k=8 n=4099 glibc_default_fresh"
        FAIL_REGULAR_EXPRESSION "CORRECTNESS FAILURE|cell failed|invalid cell|did not run as labelled"
    )
endif()
