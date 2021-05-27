#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

# Establishes build configuration modes we can use when compiling.

# Create an ASAN build variant

# Clang and GCC have slightly different linker names for the ASAN library.
set(libasan)
if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
    set(libasan "-static-libsan")
else()
    set(libasan "-static-libasan")
endif()

set(CMAKE_C_FLAGS_ASAN
    "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C compiler for ASan build type or configuration." FORCE)

set(CMAKE_CXX_FLAGS_ASAN
    "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C++ compiler for ASan build type or configuration." FORCE)

set(CMAKE_EXE_LINKER_FLAGS_ASAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address ${libasan}" CACHE STRING
    "Linker flags to be used to create executables for ASan build type." FORCE)

set(CMAKE_SHARED_LINKER_FLAGS_ASAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address ${libasan}" CACHE STRING
    "Linker lags to be used to create shared libraries for ASan build type." FORCE)

mark_as_advanced(
    CMAKE_CXX_FLAGS_ASAN
    CMAKE_C_FLAGS_ASAN
    CMAKE_EXE_LINKER_FLAGS_ASAN
    CMAKE_SHARED_LINKER_FLAGS_ASAN
)

# Create an UBSAN build variant

# Clang doesn't need to link ubsan, this is only a GCC requirement.
set(libubsan "")
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    set(libubsan "-lubsan")
endif()

set(CMAKE_C_FLAGS_UBSAN
    "${CMAKE_C_FLAGS_DEBUG} -fsanitize=undefined -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C compiler for UBSan build type or configuration." FORCE)

set(CMAKE_CXX_FLAGS_UBSAN
    "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=undefined -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C++ compiler for UBSan build type or configuration." FORCE)

set(CMAKE_EXE_LINKER_FLAGS_UBSAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined ${libubsan}" CACHE STRING
    "Linker flags to be used to create executables for UBSan build type." FORCE)

set(CMAKE_SHARED_LINKER_FLAGS_UBSAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined ${libubsan}" CACHE STRING
    "Linker lags to be used to create shared libraries for UBSan build type." FORCE)

mark_as_advanced(
    CMAKE_CXX_FLAGS_UBSAN
    CMAKE_C_FLAGS_UBSAN
    CMAKE_EXE_LINKER_FLAGS_UBSAN
    CMAKE_SHARED_LINKER_FLAGS_UBSAN
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "None" CACHE STRING "Choose the type of build, options are: None Debug Release ASan UBSan." FORCE)
endif()

set(CMAKE_CONFIGURATION_TYPES None Debug Release ASan UBSan)
