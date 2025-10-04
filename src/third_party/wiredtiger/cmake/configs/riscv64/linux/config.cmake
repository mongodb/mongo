include(CheckCCompilerFlag)

set(WT_ARCH "riscv64" CACHE STRING "")
set(WT_OS "linux" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. Access to 'pthread_setname_np' on Linux. Append this macro to our compiler flags for Linux-based
# builds.
add_cmake_flag(CMAKE_C_FLAGS -D_GNU_SOURCE)

# See https://www.sifive.com/blog/all-aboard-part-1-compiler-args
# for background on the `rv64imafdc` and `lp64d` arguments here.
add_cmake_flag(CMAKE_C_FLAGS -march=rv64imafdc)
add_cmake_flag(CMAKE_C_FLAGS -mabi=lp64d)
