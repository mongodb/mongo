set(WT_ARCH "x86" CACHE STRING "")
set(WT_OS "linux" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. Access to 'pthread_setname_np' on Linux. Append this macro to our compiler flags for Linux-based
# builds.
add_cmake_flag(CMAKE_C_FLAGS -D_GNU_SOURCE)

# Enable x86 SIMD instrinsics when available.
CHECK_INCLUDE_FILE("x86intrin.h" has_x86intrin)
if(has_x86intrin)
    add_cmake_flag(CMAKE_C_FLAGS -DHAVE_X86INTRIN_H)
endif()
unset(has_x86intrin CACHE)
