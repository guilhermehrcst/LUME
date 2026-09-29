cmake_minimum_required(VERSION 3.20)

project(
    PXIR
    VERSION 0.1.0
    DESCRIPTION "Experimental intermediate representation research"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PXIR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in PXIR C++ targets" OFF)
option(PXIR_BUILD_BENCHMARKS "Build PXIR benchmarks" ON)
set(PXIR_SANITIZE "" CACHE STRING "Comma-separated sanitizers for GCC/Clang, e.g. address,undefined")

if(PXIR_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "PXIR_SANITIZE is supported only with GCC/Clang")
    endif()
    add_compile_options(-fsanitize=${PXIR_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${PXIR_SANITIZE})
endif()

function(pxir_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(PXIR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
        if(PXIR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

include(CTest)

if(BUILD_TESTING)
    enable_testing()
endif()

add_subdirectory(experiments/001-data-types)
add_subdirectory(src)
add_subdirectory(oracle)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(PXIR_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
