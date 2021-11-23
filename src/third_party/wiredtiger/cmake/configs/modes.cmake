#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

# Establishes build configuration modes we can use when compiling.

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)

# Establish an internal cache variable to track our custom build modes.
set(BUILD_MODES None Debug Release CACHE INTERNAL "")

if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(MSVC_C_COMPILER 1)
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "^(Apple)?(C|c?)lang")
    set(CLANG_C_COMPILER 1)
else()
    set(GNU_C_COMPILER 1)
endif()

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    set(MSVC_CXX_COMPILER 1)
elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "^(Apple)?(C|c?)lang")
    set(CLANG_CXX_COMPILER 1)
else()
    set(GNU_CXX_COMPILER 1)
endif()

function(define_build_mode mode)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "DEFINE_BUILD"
        ""
        "DEPENDS"
        "C_COMPILER_FLAGS;CXX_COMPILER_FLAGS;LINK_FLAGS"
    )
    if (NOT "${DEFINE_BUILD_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to define_build_mode: ${DEFINE_BUILD_UNPARSED_ARGUMENTS}")
    endif()

    # Test if dependencies are met. Skip defining the build mode if not.
    eval_dependency("${DEFINE_BUILD_DEPENDS}" enabled)
    if(NOT enabled)
        message(VERBOSE "Skipping build mode definition due to unmet dependencies: ${mode}")
        return()
    endif()

    # Needs to validate linker flags to assert its a valid build mode.
    if(DEFINE_BUILD_LINK_FLAGS)
        set(CMAKE_REQUIRED_FLAGS "${DEFINE_BUILD_LINK_FLAGS}")
    endif()
    # Check if the compiler flags are available to ensure its a valid build mode.
    if(DEFINE_BUILD_C_COMPILER_FLAGS)
        check_c_compiler_flag("${DEFINE_BUILD_C_COMPILER_FLAGS}" HAVE_BUILD_MODE_C_FLAGS)
        if(NOT HAVE_BUILD_MODE_C_FLAGS)
            message(VERBOSE "Skipping build mode definition due to unavailable C flags: ${mode}")
            return()
        endif()
    endif()
    if(DEFINE_BUILD_CXX_COMPILER_FLAGS)
        check_cxx_compiler_flag("${DEFINE_BUILD_CXX_COMPILER_FLAGS}" HAVE_BUILD_MODE_CXX_FLAGS)
        if(NOT HAVE_BUILD_MODE_CXX_FLAGS)
            message(VERBOSE "Skipping build mode definition due to unavailable CXX flags: ${mode}")
            return()
        endif()
    endif()
    unset(CMAKE_REQUIRED_FLAGS)
    unset(HAVE_BUILD_MODE_C_FLAGS)
    unset(HAVE_BUILD_MODE_CXX_FLAGS)

    string(REPLACE ";" " " DEFINE_BUILD_C_COMPILER_FLAGS "${DEFINE_BUILD_C_COMPILER_FLAGS}")
    string(REPLACE ";" " " DEFINE_BUILD_CXX_COMPILER_FLAGS "${DEFINE_BUILD_CXX_COMPILER_FLAGS}")
    string(REPLACE ";" " " DEFINE_BUILD_LINK_FLAGS "${DEFINE_BUILD_LINK_FLAGS}")
    string(TOUPPER ${mode} build_mode)
    set(CMAKE_C_FLAGS_${build_mode}
        "${DEFINE_BUILD_C_COMPILER_FLAGS}" CACHE STRING
        "Flags used by the C compiler for ${mode} build type or configuration." FORCE)

    set(CMAKE_CXX_FLAGS_${build_mode}
        "${DEFINE_BUILD_CXX_COMPILER_FLAGS}" CACHE STRING
        "Flags used by the C++ compiler for ${mode} build type or configuration." FORCE)

    set(CMAKE_EXE_LINKER_FLAGS_${build_mode}
        "${DEFINE_BUILD_LINK_FLAGS}" CACHE STRING
        "Linker flags to be used to create executables for ${mode} build type." FORCE)

    set(CMAKE_SHARED_LINKER_FLAGS_${build_mode}
        "${DEFINE_BUILD_LINK_FLAGS}" CACHE STRING
        "Linker lags to be used to create shared libraries for ${mode} build type." FORCE)

    mark_as_advanced(
        CMAKE_CXX_FLAGS_${build_mode}
        CMAKE_C_FLAGS_${build_mode}
        CMAKE_EXE_LINKER_FLAGS_${build_mode}
        CMAKE_SHARED_LINKER_FLAGS_${build_mode}
    )
    set(BUILD_MODES "${BUILD_MODES};${mode}" CACHE INTERNAL "")
endfunction()

if(MSVC)
    set(no_omit_frame_flag "/Oy-")
else()
    set(no_omit_frame_flag "-fno-omit-frame-pointer")
endif()

# ASAN build variant flags.
if(MSVC)
    set(asan_link_flags "/fsanitize=address")
    set(asan_compiler_c_flag "/fsanitize=address")
    set(asan_compiler_cxx_flag "/fsanitize=address")
else()
    set(asan_link_flags "-fsanitize=address")
    set(asan_compiler_c_flag "-fsanitize=address")
    set(asan_compiler_cxx_flag "-fsanitize=address")
endif()

# UBSAN build variant flags.
set(ubsan_link_flags "-fsanitize=undefined")
set(ubsan_compiler_c_flag "-fsanitize=undefined")
set(ubsan_compiler_cxx_flag "-fsanitize=undefined")

# MSAN build variant flags.
set(msan_link_flags "-fsanitize=memory")
set(msan_compiler_c_flag "-fsanitize=memory" "-fno-optimize-sibling-calls")
set(msan_compiler_cxx_flag "-fsanitize=memory" "-fno-optimize-sibling-calls")

# TSAN build variant flags.
set(tsan_link_flags "-fsanitize=thread")
set(tsan_compiler_c_flag "-fsanitize=thread")
set(tsan_compiler_cxx_flag "-fsanitize=thread")

# Define our custom build variants.
define_build_mode(ASan
    C_COMPILER_FLAGS ${asan_compiler_c_flag}
    CXX_COMPILER_FLAGS ${asan_compiler_cxx_flag}
    LINK_FLAGS ${asan_link_flags}
)

define_build_mode(UBSan
    C_COMPILER_FLAGS ${ubsan_compiler_c_flag} ${no_omit_frame_flag}
    CXX_COMPILER_FLAGS ${ubsan_compiler_cxx_flag} ${no_omit_frame_flag}
    LINK_FLAGS ${ubsan_link_flags}
    # Disable UBSan on MSVC compilers (unsupported).
    DEPENDS "NOT MSVC"
)

define_build_mode(MSan
    C_COMPILER_FLAGS ${msan_compiler_c_flag} ${no_omit_frame_flag}
    CXX_COMPILER_FLAGS ${msan_compiler_cxx_flag}
    LINK_FLAGS ${msan_link_flags}
    # Disable MSan on MSVC and GNU compilers (unsupported).
    DEPENDS "CLANG_C_COMPILER"
)

define_build_mode(TSan
    C_COMPILER_FLAGS ${tsan_compiler_c_flag} ${no_omit_frame_flag}
    CXX_COMPILER_FLAGS ${tsan_compiler_cxx_flag}
    LINK_FLAGS ${tsan_link_flags}
    # Disable TSan on MSVC compilers (unsupported).
    DEPENDS "NOT MSVC"
)

if(NOT CMAKE_BUILD_TYPE)
    string(REPLACE ";" " " build_modes_doc "${BUILD_MODES}")
    set(CMAKE_BUILD_TYPE "None" CACHE STRING "Choose the type of build, options are: ${build_modes_doc}." FORCE)
endif()

if(CMAKE_BUILD_TYPE)
    if(NOT "${CMAKE_BUILD_TYPE}" IN_LIST BUILD_MODES)
        message(FATAL_ERROR "Build type '${CMAKE_BUILD_TYPE}' not available.")
    endif()
endif()

set(CMAKE_CONFIGURATION_TYPES ${BUILD_MODES})
