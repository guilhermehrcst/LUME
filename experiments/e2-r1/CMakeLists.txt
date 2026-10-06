# Experiment E2-R1 (cross-machine replication): secondary PMU characterization binary.
# Linux only (perf_event_open, E1 hooks and workspace). The primary timing binary is
# lume_e2_bench from the E2 experiment, unmodified.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "E2-R1 is Linux-only; skipped on ${CMAKE_SYSTEM_NAME}")
    return()
endif()

add_executable(lume_e2r1_pmu e2r1_pmu.cpp)
target_include_directories(lume_e2r1_pmu PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(lume_e2r1_pmu PRIVATE lume lume_oracle lume_e1_hooks)
lume_set_warnings(lume_e2r1_pmu)

if(BUILD_TESTING)
    # Smoke only: unsupported events must be reported, not fatal, and both strategies are oracle-checked.
    add_test(NAME lume_e2r1_pmu_smoke
        COMMAND lume_e2r1_pmu out=${CMAKE_CURRENT_BINARY_DIR}/smoke_pmu.csv compiler=smoke replicate=smoke
                events=cycles,instructions,page-faults,LLC-load-misses dtypes=f32,i32 ks=1,3 sizes=64,4096 reps=2 warmup=1)
    set_tests_properties(lume_e2r1_pmu_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "done i32 k=3 n=4096"
        FAIL_REGULAR_EXPRESSION "CORRECTNESS FAILURE|cell failed|did not run as labelled|unknown event")
endif()

# Self-test of the cross-machine analysis (synthetic bundles with known verdicts, the committed E2 data
# through the validator, and the disassembly parser on a kernel fixture). Not performance data.
find_program(LUME_PYTHON3 python3)
if(BUILD_TESTING AND LUME_PYTHON3)
    add_test(NAME lume_e2r1_analysis_selftest COMMAND ${LUME_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/test_analyze_cross_machine.py)
    set_tests_properties(lume_e2r1_analysis_selftest PROPERTIES LABELS "lume;correctness" TIMEOUT 300)
endif()

if(BUILD_TESTING)
    add_test(NAME lume_e2r1_physical_host_detector
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_check_physical_host.sh)
    set_tests_properties(lume_e2r1_physical_host_detector PROPERTIES
        LABELS "lume;correctness"
        PASS_REGULAR_EXPRESSION "physical host dmesg classifier regression tests passed")
endif()
