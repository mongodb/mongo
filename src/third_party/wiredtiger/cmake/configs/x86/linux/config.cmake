set(WT_ARCH "x86" CACHE STRING "")
set(WT_OS "linux" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. Access to O_DIRECT on Linux. Append this macro to our compiler flags for Linux-based
# builds.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")

# Linux requires buffers aligned to 4KB boundaries for O_DIRECT to work.
set(WT_BUFFER_ALIGNMENT_DEFAULT "4096" CACHE STRING "")

# Enable x86 SIMD instrinsics when available.
CHECK_INCLUDE_FILE("x86intrin.h" has_x86intrin)
if(has_x86intrin)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DHAVE_X86INTRIN_H" CACHE STRING "" FORCE)
endif()
unset(has_x86intrin CACHE)
