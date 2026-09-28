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
endif()
