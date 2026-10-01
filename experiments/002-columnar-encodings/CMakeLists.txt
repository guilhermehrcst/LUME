# Experiment 002 is self-contained: it does not link the lume library.
execute_process(
    COMMAND git -C ${PROJECT_SOURCE_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE LUME_EXP002_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT LUME_EXP002_GIT_SHA)
    set(LUME_EXP002_GIT_SHA "unknown")
endif()

add_executable(lume_exp_002 src/main.cpp)
target_include_directories(lume_exp_002 PRIVATE src)
target_compile_definitions(lume_exp_002 PRIVATE
    "LUME_BUILD_CONFIG=\"$<CONFIG>\""
    "LUME_GIT_SHA=\"${LUME_EXP002_GIT_SHA}\""
)
lume_set_warnings(lume_exp_002)

add_executable(lume_exp_002_roundtrip src/roundtrip_test.cpp)
target_include_directories(lume_exp_002_roundtrip PRIVATE src)
lume_set_warnings(lume_exp_002_roundtrip)

if(BUILD_TESTING)
    add_test(NAME exp002_roundtrip COMMAND lume_exp_002_roundtrip)
    set_tests_properties(exp002_roundtrip PROPERTIES
        LABELS "lume;correctness"
        PASS_REGULAR_EXPRESSION "exp002_roundtrip=PASS"
        FAIL_REGULAR_EXPRESSION "FAIL"
    )

    # Smoke test only: proves the benchmark runs and its correctness gate
    # passes. Timings from CI are not performance evidence.
    add_test(NAME exp002_smoke COMMAND lume_exp_002 rows=20000 seed=1 rounds=3)
    set_tests_properties(exp002_smoke PROPERTIES
        LABELS "lume;benchmark"
        PASS_REGULAR_EXPRESSION "correctness=exact"
        FAIL_REGULAR_EXPRESSION "MISMATCH"
    )
endif()
