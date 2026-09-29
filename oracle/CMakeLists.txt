# Correctness oracle. Must not link pxir: its independence from the code under
# test is the point.
add_library(pxir_oracle INTERFACE)
target_include_directories(pxir_oracle INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pxir_oracle INTERFACE cxx_std_20)
