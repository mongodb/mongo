include(CheckCCompilerFlag)
include(cmake/rcpc_test.cmake)

set(WT_ARCH "aarch64" CACHE STRING "")
set(WT_OS "darwin" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# ARMv8-A is the 64-bit ARM architecture, turn on the optional CRC.
# If the compilation check in rcpc_test passes also turn on the RCpc instructions.
if(HAVE_RCPC)
    add_cmake_flag(CMAKE_C_FLAGS -march=armv8.2-a+rcpc+crc)
    add_cmake_flag(CMAKE_CXX_FLAGS -march=armv8.2-a+rcpc+crc)
else()
    add_cmake_flag(CMAKE_C_FLAGS -march=armv8-a+crc)
    add_cmake_flag(CMAKE_CXX_FLAGS -march=armv8-a+crc)
endif()

# Disable cppsuite as it only runs on linux.
set(ENABLE_CPPSUITE 0)
check_c_compiler_flag("-moutline-atomics" has_moutline_atomics)

# moutline-atomics preserves backwards compatibility with Arm v8.0 systems but also supports
# using Arm v8.1 atomics. The latter can massively improve performance on larger Arm systems.
# The flag was back ported to gcc8, 9 and is the default in gcc10+. See if the compiler supports
# the flag.
if(has_moutline_atomics)
    add_cmake_flag(CMAKE_C_FLAGS -moutline-atomics)
endif()
unset(has_moutline_atomics CACHE)

# Header file here is required for portable futex implementation.
if(NOT CMAKE_CROSSCOMPILING)
    include_directories(AFTER SYSTEM "${CMAKE_SOURCE_DIR}/oss/apple")
endif()
