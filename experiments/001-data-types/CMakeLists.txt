add_executable(pxir_exp_001 src/main.c)

if(MSVC)
    target_compile_options(pxir_exp_001 PRIVATE /W4)
else()
    target_compile_options(pxir_exp_001 PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_test(NAME exp001_u8 COMMAND pxir_exp_001 u8 1024)
    add_test(NAME exp001_u16 COMMAND pxir_exp_001 u16 1024)
    add_test(NAME exp001_u32 COMMAND pxir_exp_001 u32 1024)
    add_test(NAME exp001_u64 COMMAND pxir_exp_001 u64 1024)

    set_tests_properties(exp001_u8 PROPERTIES PASS_REGULAR_EXPRESSION "requested_bytes=1024")
    set_tests_properties(exp001_u16 PROPERTIES PASS_REGULAR_EXPRESSION "requested_bytes=2048")
    set_tests_properties(exp001_u32 PROPERTIES PASS_REGULAR_EXPRESSION "requested_bytes=4096")
    set_tests_properties(exp001_u64 PROPERTIES PASS_REGULAR_EXPRESSION "requested_bytes=8192")

    add_test(NAME exp001_rejects_zero COMMAND pxir_exp_001 u8 0)
    add_test(NAME exp001_rejects_negative COMMAND pxir_exp_001 u8 -1)
    add_test(NAME exp001_rejects_non_numeric COMMAND pxir_exp_001 u8 nope)
    add_test(NAME exp001_rejects_unknown_type COMMAND pxir_exp_001 u128 1024)

    set_tests_properties(
        exp001_rejects_zero
        exp001_rejects_negative
        exp001_rejects_non_numeric
        exp001_rejects_unknown_type
        PROPERTIES WILL_FAIL TRUE
    )
endif()
