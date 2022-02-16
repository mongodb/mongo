cmake_minimum_required(VERSION 3.10.0)

if(NOT TOOLCHAIN_ROOT)
    set(TOOLCHAIN_ROOT "/opt/mongodbtoolchain/v4")
endif()

set(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_ROOT}/bin/gcc")
