#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

# Establishes build configuration modes we can use when compiling.

# Create an ASAN build variant
set(CMAKE_C_FLAGS_ASAN
    "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C compiler for ASan build type or configuration." FORCE)

set(CMAKE_CXX_FLAGS_ASAN
    "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C++ compiler for ASan build type or configuration." FORCE)

set(CMAKE_EXE_LINKER_FLAGS_ASAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address -static-libasan" CACHE STRING
    "Linker flags to be used to create executables for ASan build type." FORCE)

set(CMAKE_SHARED_LINKER_FLAGS_ASAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address -static-libasan" CACHE STRING
    "Linker lags to be used to create shared libraries for ASan build type." FORCE)

mark_as_advanced(
    CMAKE_CXX_FLAGS_ASAN
    CMAKE_C_FLAGS_ASAN
    CMAKE_EXE_LINKER_FLAGS_ASAN
    CMAKE_SHARED_LINKER_FLAGS_ASAN
)

# Create an UBSAN build variant
set(CMAKE_C_FLAGS_UBSAN
    "${CMAKE_C_FLAGS_DEBUG} -fsanitize=undefined -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C compiler for UBSan build type or configuration." FORCE)

set(CMAKE_CXX_FLAGS_UBSAN
    "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=undefined -fno-omit-frame-pointer" CACHE STRING
    "Flags used by the C++ compiler for UBSan build type or configuration." FORCE)

set(CMAKE_EXE_LINKER_FLAGS_UBSAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined -lubsan" CACHE STRING
    "Linker flags to be used to create executables for UBSan build type." FORCE)

set(CMAKE_SHARED_LINKER_FLAGS_UBSAN
    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined -lubsan" CACHE STRING
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
