include(CheckCCompilerFlag)

set(WT_ARCH "aarch64" CACHE STRING "")
set(WT_OS "linux" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. Access to O_DIRECT on Linux. Append this macro to our compiler flags for Linux-based
# builds.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE" CACHE STRING "" FORCE)

# Linux requires buffers aligned to 4KB boundaries for O_DIRECT to work.
set(WT_BUFFER_ALIGNMENT_DEFAULT "4096" CACHE STRING "")

# ARMv8-A is the 64-bit ARM architecture, turn on the optional CRC instructions.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crc" CACHE STRING "" FORCE)

check_c_compiler_flag("-moutline-atomics" has_moutline_atomics)

# moutline-atomics preserves backwards compatibility with Arm v8.0 systems but also supports
# using Arm v8.1 atomics. The latter can massively improve performance on larger Arm systems.
# The flag was back ported to gcc8, 9 and is the default in gcc10+. See if the compiler supports
# the flag.
if(has_moutline_atomics)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -moutline-atomics" CACHE STRING "" FORCE)
endif()
unset(has_moutline_atomics CACHE)
