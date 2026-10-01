add_library(lume STATIC
    ir/program.cpp
    ir/dump.cpp
    verify/verifier.cpp
    runtime/cpu_reference.cpp
)
target_include_directories(lume PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(lume PUBLIC cxx_std_20)
lume_set_warnings(lume)
