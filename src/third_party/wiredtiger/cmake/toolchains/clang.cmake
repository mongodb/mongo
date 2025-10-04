cmake_minimum_required(VERSION 3.10.0)

set(C_COMPILER_VERSION_SUFFIX)
set(CXX_COMPILER_VERSION_SUFFIX)
if(CLANG_C_VERSION)
    set(C_COMPILER_VERSION_SUFFIX "-${CLANG_C_VERSION}")
endif()
if(CLANG_CXX_VERSION)
    set(CXX_COMPILER_VERSION_SUFFIX "-${CLANG_CXX_VERSION}")
endif()

set(CMAKE_C_COMPILER "clang${C_COMPILER_VERSION_SUFFIX}")
set(CMAKE_C_COMPILER_ID "Clang")

set(CMAKE_CXX_COMPILER "clang++${CXX_COMPILER_VERSION_SUFFIX}")
set(CMAKE_CXX_COMPILER_ID "Clang++")

set(CMAKE_ASM_COMPILER "clang${C_COMPILER_VERSION_SUFFIX}")
set(CMAKE_ASM_COMPILER_ID "Clang")

if(NOT "${COMPILE_DEFINITIONS}" STREQUAL "")
    ### Additional check to overcome check_[symbol|include|function]_exits using toolchain file without passing WT_ARCH and WT_OS.
    string(REGEX MATCH "-DWT_ARCH=([A-Za-z0-9]+) -DWT_OS=([A-Za-z0-9]+)" _ ${COMPILE_DEFINITIONS})
    set(wt_config_arch ${CMAKE_MATCH_1})
    set(wt_config_os ${CMAKE_MATCH_2})
else()
    set(wt_config_arch ${WT_ARCH})
    set(wt_config_os ${WT_OS})
endif()

# Include any platform specific clang configurations and flags e.g. target-tuple, flags.
if((NOT "${wt_config_arch}" STREQUAL "") AND (NOT "${wt_config_os}" STREQUAL ""))
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${wt_config_arch}/${wt_config_os}/plat_clang.cmake")
        message(FATAL_ERROR "(${wt_config_arch}/${wt_config_os}) directory does not have a plat_clang.cmake file")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/${wt_config_arch}/${wt_config_os}/plat_clang.cmake")
endif()
