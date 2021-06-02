#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

# Establishes build configuration modes we can use when compiling.

include(CheckCCompilerFlag)

set(build_modes None Debug Release)
if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(no_omit_frame_flag "/Oy-")
else()
    set(no_omit_frame_flag "-fno-omit-frame-pointer")
endif()

# Create an ASAN build variant
if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(asan_link_flags "/fsanitize=address")
    set(asan_compiler_flag "/fsanitize=address")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
    set(asan_link_flags "-fsanitize=address -static-libsan")
    set(asan_compiler_flag "-fsanitize=address")
else()
    set(asan_link_flags "-fsanitize=address -static-libasan")
    set(asan_compiler_flag "-fsanitize=address")
endif()

# Needs to validate linker flags for the test to also pass.
set(CMAKE_REQUIRED_FLAGS "${asan_link_flags}")
# Check if the ASAN compiler flag is available.
check_c_compiler_flag("${asan_compiler_flag}" HAVE_ADDRESS_SANITIZER)
unset(CMAKE_REQUIRED_FLAGS)

if(HAVE_ADDRESS_SANITIZER)
    set(CMAKE_C_FLAGS_ASAN
        "${CMAKE_C_FLAGS_DEBUG} ${asan_compiler_flag} ${no_omit_frame_flag}" CACHE STRING
        "Flags used by the C compiler for ASan build type or configuration." FORCE)

    set(CMAKE_CXX_FLAGS_ASAN
        "${CMAKE_CXX_FLAGS_DEBUG} ${asan_compiler_flag} ${no_omit_frame_flag}" CACHE STRING
        "Flags used by the C++ compiler for ASan build type or configuration." FORCE)

    set(CMAKE_EXE_LINKER_FLAGS_ASAN
        "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} ${asan_link_flags}" CACHE STRING
        "Linker flags to be used to create executables for ASan build type." FORCE)

    set(CMAKE_SHARED_LINKER_FLAGS_ASAN
        "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} ${asan_link_flags}" CACHE STRING
        "Linker lags to be used to create shared libraries for ASan build type." FORCE)

    mark_as_advanced(
        CMAKE_CXX_FLAGS_ASAN
        CMAKE_C_FLAGS_ASAN
        CMAKE_EXE_LINKER_FLAGS_ASAN
        CMAKE_SHARED_LINKER_FLAGS_ASAN
    )
    list(APPEND build_modes "ASan")
endif()

# Create an UBSAN build variant
if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(ubsan_link_flags "/fsanitize=undefined")
    set(ubsan_compiler_flag "/fsanitize=undefined")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
    set(ubsan_link_flags "-fsanitize=undefined")
    set(ubsan_compiler_flag "-fsanitize=undefined")
else()
    set(ubsan_link_flags "-fsanitize=undefined -lubsan")
    set(ubsan_compiler_flag "-fsanitize=undefined")
endif()

# Needs to validate linker flags for the test to also pass.
set(CMAKE_REQUIRED_FLAGS "${ubsan_link_flags}")
# Check if the UBSAN compiler flag is available.
check_c_compiler_flag("${ubsan_compiler_flag}" HAVE_UB_SANITIZER)
unset(CMAKE_REQUIRED_FLAGS)

if(HAVE_UB_SANITIZER)
    set(CMAKE_C_FLAGS_UBSAN
        "${CMAKE_C_FLAGS_DEBUG} ${ubsan_compiler_flag} ${no_omit_frame_flag}" CACHE STRING
        "Flags used by the C compiler for UBSan build type or configuration." FORCE)

    set(CMAKE_CXX_FLAGS_UBSAN
        "${CMAKE_CXX_FLAGS_DEBUG} ${ubsan_compiler_flag} ${no_omit_frame_flag}" CACHE STRING
        "Flags used by the C++ compiler for UBSan build type or configuration." FORCE)

    set(CMAKE_EXE_LINKER_FLAGS_UBSAN
        "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} ${ubsan_link_flags}" CACHE STRING
        "Linker flags to be used to create executables for UBSan build type." FORCE)

    set(CMAKE_SHARED_LINKER_FLAGS_UBSAN
        "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} ${ubsan_link_flags}" CACHE STRING
        "Linker lags to be used to create shared libraries for UBSan build type." FORCE)

    mark_as_advanced(
        CMAKE_CXX_FLAGS_UBSAN
        CMAKE_C_FLAGS_UBSAN
        CMAKE_EXE_LINKER_FLAGS_UBSAN
        CMAKE_SHARED_LINKER_FLAGS_UBSAN
    )
    list(APPEND build_modes "UBSan")
endif()
if(NOT CMAKE_BUILD_TYPE)
    string(REPLACE ";" " " build_modes_doc "${build_modes}")
    set(CMAKE_BUILD_TYPE "None" CACHE STRING "Choose the type of build, options are: ${build_modes_doc}." FORCE)
endif()

set(CMAKE_CONFIGURATION_TYPES ${build_modes})
