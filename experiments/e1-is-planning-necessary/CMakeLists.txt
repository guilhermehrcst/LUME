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
