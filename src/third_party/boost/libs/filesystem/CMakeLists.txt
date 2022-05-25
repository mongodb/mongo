# Copyright 2019 Mike Dev
# Copyright 2020-2022 Andrey Semashev
#
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
#
# NOTE: CMake support for Boost.Filesystem is currently experimental at best
#       and the interface is likely to change in the future

cmake_minimum_required(VERSION 3.5)
project(BoostFilesystem VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX)

include(CheckCXXSourceCompiles)

set(BOOST_FILESYSTEM_NO_DEPRECATED OFF CACHE BOOL "Disable deprecated functionality of Boost.Filesystem")
set(BOOST_FILESYSTEM_DISABLE_SENDFILE OFF CACHE BOOL "Disable usage of sendfile API in Boost.Filesystem")
set(BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE OFF CACHE BOOL "Disable usage of copy_file_range API in Boost.Filesystem")
set(BOOST_FILESYSTEM_DISABLE_STATX OFF CACHE BOOL "Disable usage of statx API in Boost.Filesystem")
set(BOOST_FILESYSTEM_DISABLE_GETRANDOM OFF CACHE BOOL "Disable usage of getrandom API in Boost.Filesystem")
set(BOOST_FILESYSTEM_DISABLE_ARC4RANDOM OFF CACHE BOOL "Disable usage of arc4random API in Boost.Filesystem")
set(BOOST_FILESYSTEM_DISABLE_BCRYPT OFF CACHE BOOL "Disable usage of BCrypt API in Boost.Filesystem")

# Note: We can't use the Boost::library targets in the configure checks as they may not yet be included
# by the superproject when this CMakeLists.txt is included. We also don't want to hardcode include paths
# of the needed libraries and their dependencies, recursively, as this is too fragile and requires maintenance.
# Instead, we collect include paths of all libraries and use them in the configure checks. This works faster
# if there is a unified Boost include tree in the filesystem (i.e. if `b2 headers` was run or we're in the
# official monolithic Boost distribution tree).
include(cmake/BoostLibraryIncludes.cmake)

set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src" ${BOOST_LIBRARY_INCLUDES})

check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_attribute_init_priority.cpp>" BOOST_FILESYSTEM_HAS_INIT_PRIORITY)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_cxx20_atomic_ref.cpp>" BOOST_FILESYSTEM_HAS_CXX20_ATOMIC_REF)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_blksize.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_BLKSIZE)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_mtim.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_mtimensec.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_mtimespec.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_birthtimensec.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMENSEC)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_birthtimespec.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMESPEC)
if(NOT BOOST_FILESYSTEM_DISABLE_STATX)
    check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_statx.cpp>" BOOST_FILESYSTEM_HAS_STATX)
    if(NOT BOOST_FILESYSTEM_HAS_STATX)
        check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_statx_syscall.cpp>" BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
    endif()
endif()
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_fdopendir_nofollow.cpp>" BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
if(WIN32 AND NOT BOOST_FILESYSTEM_DISABLE_BCRYPT)
    set(CMAKE_REQUIRED_LIBRARIES bcrypt)
    check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_bcrypt.cpp>" BOOST_FILESYSTEM_HAS_BCRYPT)
    unset(CMAKE_REQUIRED_LIBRARIES)
endif()

unset(CMAKE_REQUIRED_INCLUDES)

set(BOOST_FILESYSTEM_SOURCES
    src/codecvt_error_category.cpp
    src/exception.cpp
    src/operations.cpp
    src/directory.cpp
    src/path.cpp
    src/path_traits.cpp
    src/portability.cpp
    src/unique_path.cpp
    src/utf8_codecvt_facet.cpp
)
if(WIN32 OR CYGWIN)
    list(APPEND BOOST_FILESYSTEM_SOURCES src/windows_file_codecvt.cpp)
endif()

add_library(boost_filesystem ${BOOST_FILESYSTEM_SOURCES})
add_library(Boost::filesystem ALIAS boost_filesystem)

target_include_directories(boost_filesystem PUBLIC include)
target_include_directories(boost_filesystem PRIVATE src)

target_compile_definitions(boost_filesystem
    PUBLIC
        # NOTE:
        # We deactivate autolinking, because cmake based builds don't need it
        # and we don't implement name mangling for the library file anyway.
        # Ususally the parent CMakeLists.txt file should already have globally defined BOOST_ALL_NO_LIB
        BOOST_FILESYSTEM_NO_LIB
        $<$<STREQUAL:$<TARGET_PROPERTY:boost_filesystem,TYPE>,SHARED_LIBRARY>:BOOST_FILESYSTEM_DYN_LINK=1>
        $<$<STREQUAL:$<TARGET_PROPERTY:boost_filesystem,TYPE>,STATIC_LIBRARY>:BOOST_FILESYSTEM_STATIC_LINK=1>

    PRIVATE
        BOOST_FILESYSTEM_SOURCE
)

if(WIN32)
    target_compile_definitions(boost_filesystem PRIVATE
        _SCL_SECURE_NO_WARNINGS
        _SCL_SECURE_NO_DEPRECATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_SECURE_NO_DEPRECATE
    )
endif()

if(WIN32 OR CYGWIN)
    target_compile_definitions(boost_filesystem PRIVATE
        BOOST_USE_WINDOWS_H
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

if(BOOST_FILESYSTEM_NO_DEPRECATED)
    target_compile_definitions(boost_filesystem PUBLIC BOOST_FILESYSTEM_NO_DEPRECATED)
endif()

if(BOOST_FILESYSTEM_HAS_INIT_PRIORITY)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_INIT_PRIORITY)
endif()

if(BOOST_FILESYSTEM_DISABLE_SENDFILE)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_DISABLE_SENDFILE)
endif()
if(BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_DISABLE_COPY_FILE_RANGE)
endif()
if(BOOST_FILESYSTEM_DISABLE_STATX)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_DISABLE_STATX)
endif()
if(BOOST_FILESYSTEM_DISABLE_GETRANDOM)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_DISABLE_GETRANDOM)
endif()
if(BOOST_FILESYSTEM_DISABLE_ARC4RANDOM)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_DISABLE_ARC4RANDOM)
endif()
if(BOOST_FILESYSTEM_DISABLE_BCRYPT)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_DISABLE_BCRYPT)
endif()

if(BOOST_FILESYSTEM_HAS_STAT_ST_BLKSIZE)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_BLKSIZE)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMENSEC)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMENSEC)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMESPEC)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_BIRTHTIMESPEC)
endif()
if(BOOST_FILESYSTEM_HAS_STATX)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STATX)
endif()
if(BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STATX_SYSCALL)
endif()
if(BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_FDOPENDIR_NOFOLLOW)
endif()

target_link_libraries(boost_filesystem
    PUBLIC
        Boost::assert
        Boost::config
        Boost::container_hash
        Boost::core
        Boost::detail
        Boost::io
        Boost::iterator
        Boost::smart_ptr
        Boost::system
        Boost::type_traits

    PRIVATE
        Boost::predef
)

if(NOT BOOST_FILESYSTEM_HAS_CXX20_ATOMIC_REF)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF)
    target_link_libraries(boost_filesystem
        PRIVATE
            Boost::atomic
    )
endif()

if(WIN32)
    if(BOOST_FILESYSTEM_HAS_BCRYPT)
        target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_BCRYPT)
        target_link_libraries(boost_filesystem PRIVATE bcrypt)
    else()
        target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_WINCRYPT)
        if(NOT WINCE)
            target_link_libraries(boost_filesystem PRIVATE advapi32)
        else()
            target_link_libraries(boost_filesystem PRIVATE coredll)
        endif()
    endif()

    target_link_libraries(boost_filesystem
        PRIVATE
            Boost::winapi
    )
endif()
