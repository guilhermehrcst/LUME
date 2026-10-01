# Correctness oracle. Must not link lume: its independence from the code under
# test is the point.
add_library(lume_oracle INTERFACE)
target_include_directories(lume_oracle INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lume_oracle INTERFACE cxx_std_20)
