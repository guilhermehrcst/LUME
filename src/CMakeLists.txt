add_library(pxir STATIC
    ir/program.cpp
    ir/dump.cpp
    verify/verifier.cpp
    runtime/cpu_reference.cpp
)
target_include_directories(pxir PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(pxir PUBLIC cxx_std_20)
pxir_set_warnings(pxir)
