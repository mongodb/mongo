# See https://www.sifive.com/blog/all-aboard-part-1-compiler-args
# for background on the `rv64imafdc` and `lp64d` arguments here.
add_cmake_flag(CMAKE_C_FLAGS -march=rv64imafdc)
add_cmake_flag(CMAKE_C_FLAGS -mabi=lp64d)
