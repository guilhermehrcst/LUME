cmake_minimum_required(VERSION 3.20)

project(
    PXIR
    VERSION 0.1.0
    DESCRIPTION "Experimental intermediate representation research"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

if(BUILD_TESTING)
    enable_testing()
endif()

add_subdirectory(experiments/001-data-types)
