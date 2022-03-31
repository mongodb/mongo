set(WT_ARCH "x86" CACHE STRING "")
set(WT_OS "darwin" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Enable x86 SIMD instrinsics when available.
CHECK_INCLUDE_FILE("x86intrin.h" has_x86intrin)
if(has_x86intrin)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DHAVE_X86INTRIN_H" CACHE STRING "" FORCE)
endif()
unset(has_x86intrin CACHE)
