#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

cmake_minimum_required(VERSION 3.10.0)

if(NOT "${COMPILE_DEFINITIONS}" STREQUAL "")
    ### Additional check to overcome check_[symbol|include|function]_exits using toolchain file without passing WT_ARCH and WT_OS.
    string(REGEX MATCH "-DWT_ARCH=([A-Za-z0-9]+) -DWT_OS=([A-Za-z0-9]+)" _ ${COMPILE_DEFINITIONS})
    set(wt_config_arch ${CMAKE_MATCH_1})
    set(wt_config_os ${CMAKE_MATCH_2})
else()
    set(wt_config_arch ${WT_ARCH})
    set(wt_config_os ${WT_OS})
endif()

# Include any platform specific gcc configurations and flags e.g. target-tuple, flags.
if((NOT "${wt_config_arch}" STREQUAL "") AND (NOT "${wt_config_os}" STREQUAL ""))
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${wt_config_arch}/${wt_config_os}/plat_gcc.cmake")
        message(FATAL_ERROR "(${wt_config_arch}/${wt_config_os}) directory does not have a plat_gcc.cmake file")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/${wt_config_arch}/${wt_config_os}/plat_gcc.cmake")
endif()

set(C_COMPILER_VERSION_SUFFIX)
set(CXX_COMPILER_VERSION_SUFFIX)
if(GNU_C_VERSION)
    set(C_COMPILER_VERSION_SUFFIX "-${GNU_C_VERSION}")
endif()
if(GNU_CXX_VERSION)
    set(CXX_COMPILER_VERSION_SUFFIX "-${GNU_CXX_VERSION}")
endif()

set(CMAKE_C_COMPILER "${CROSS_COMPILER_PREFIX}gcc${C_COMPILER_VERSION_SUFFIX}")
set(CMAKE_CXX_COMPILER "${CROSS_COMPILER_PREFIX}g++${CXX_COMPILER_VERSION_SUFFIX}")
set(CMAKE_ASM_COMPILER "${CROSS_COMPILER_PREFIX}gcc${C_COMPILER_VERSION_SUFFIX}")
