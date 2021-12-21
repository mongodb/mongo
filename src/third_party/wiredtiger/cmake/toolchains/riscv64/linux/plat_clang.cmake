#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

if(CMAKE_CROSSCOMPILING)
    set(TRIPLE_TARGET "riscv64-unknown-linux-gnu")
    set(CROSS_COMPILER_PREFIX ${TRIPLE_TARGET}-)
    set(CMAKE_C_COMPILER_TARGET "${TRIPLE_TARGET}")
    set(CMAKE_CXX_COMPILER_TARGET "${TRIPLE_TARGET}")
    set(CMAKE_ASM_COMPILER_TARGET "${TRIPLE_TARGET}")
endif()
