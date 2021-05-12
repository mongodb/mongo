# Copyright 2019 Mike Dev
# Copyright 2020 Andrey Semashev
#
# Distributed under the Boost Software License, Version 1.0.
# See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
#
# NOTE: CMake support for Boost.Filesystem is currently experimental at best
#       and the interface is likely to change in the future

cmake_minimum_required(VERSION 3.5)
project(BoostFilesystem VERSION "${BOOST_SUPERPROJECT_VERSION}" LANGUAGES CXX)

include(CheckCXXSourceCompiles)

check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_mtim.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_mtimensec.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_stat_st_mtimespec.cpp>" BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
if(WIN32)
    # Note: We can't use the Boost::library targets here as they may not yet be included by the superproject when this CMakeLists.txt is included.
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    set(CMAKE_REQUIRED_LIBRARIES bcrypt)
    check_cxx_source_compiles("#include <${CMAKE_CURRENT_SOURCE_DIR}/config/has_bcrypt.cpp>" BOOST_FILESYSTEM_HAS_BCRYPT)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_INCLUDES)
endif()

add_library(boost_filesystem
    src/codecvt_error_category.cpp
    src/exception.cpp
    src/operations.cpp
    src/directory.cpp
    src/path.cpp
    src/path_traits.cpp
    src/portability.cpp
    src/unique_path.cpp
    src/utf8_codecvt_facet.cpp
    src/windows_file_codecvt.cpp
)

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

if(BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_MTIM)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_MTIMENSEC)
endif()
if(BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
    target_compile_definitions(boost_filesystem PRIVATE BOOST_FILESYSTEM_HAS_STAT_ST_MTIMESPEC)
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
