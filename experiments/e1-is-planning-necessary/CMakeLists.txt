# Experiment E1: is planning necessary? Linux only: the workspace uses mmap and
# madvise(MADV_DONTNEED), and the harness uses glibc's malloc_usable_size.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "E1 (planning necessity) is Linux-only; skipped on ${CMAKE_SYSTEM_NAME}")
    return()
endif()

add_library(lume_e1_hooks STATIC alloc_hooks.cpp)
target_include_directories(lume_e1_hooks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
lume_set_warnings(lume_e1_hooks)

if(BUILD_TESTING)
    add_executable(lume_e1_workspace_test workspace_test.cpp)
    target_include_directories(lume_e1_workspace_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/support)
    lume_set_warnings(lume_e1_workspace_test)
    add_test(NAME lume_e1_workspace_test COMMAND lume_e1_workspace_test)
    set_tests_properties(lume_e1_workspace_test PROPERTIES LABELS "lume;correctness")

    # The hooks replace the global operator new/delete, so this test links them.
    add_executable(lume_e1_hooks_test hooks_test.cpp)
    target_include_directories(lume_e1_hooks_test PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(lume_e1_hooks_test PRIVATE lume_e1_hooks)
    lume_set_warnings(lume_e1_hooks_test)
    add_test(NAME lume_e1_hooks_test COMMAND lume_e1_hooks_test)
    set_tests_properties(lume_e1_hooks_test PROPERTIES LABELS "lume;correctness")
endif()

# The benchmark harness (Linux): not part of the default smoke set beyond the
# short run below; the full matrix is launched by run_e1.sh.
execute_process(
    COMMAND git -C ${PROJECT_SOURCE_DIR} rev-parse --short=12 HEAD
    OUTPUT_VARIABLE LUME_E1_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)
if(NOT LUME_E1_GIT_SHA)
    set(LUME_E1_GIT_SHA "unknown")
endif()
add_executable(lume_e1_bench e1_bench.cpp)
target_include_directories(lume_e1_bench PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(lume_e1_bench PRIVATE lume lume_oracle lume_e1_hooks)
target_compile_definitions(lume_e1_bench PRIVATE
    "LUME_E1_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
    "LUME_E1_FLAGS=\"${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE} (build type ${CMAKE_BUILD_TYPE})\""
    "LUME_E1_GIT_SHA=\"${LUME_E1_GIT_SHA}\""
)
lume_set_warnings(lume_e1_bench)

if(BUILD_TESTING)
    # Smoke test only: proves the harness runs every program, all three
    # in-process regimes, and that every plan is checked against the oracle.
    # Timings from CI are not performance evidence.
    add_test(NAME lume_e1_bench_smoke
        COMMAND lume_e1_bench out=${CMAKE_CURRENT_BINARY_DIR}/smoke_raw.csv plans=${CMAKE_CURRENT_BINARY_DIR}/smoke_plans.csv
                replicate=smoke regimes=glibc_default,arena_cold,arena_warm sizes=64,4099 reps=2 warmup=1)
    set_tests_properties(lume_e1_bench_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "done chain3_i32 n=4099 arena_warm"
        FAIL_REGULAR_EXPRESSION "CORRECTNESS FAILURE|cell failed|workspace exhausted|accounting mismatch"
    )
endif()
