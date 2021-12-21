#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#
include(CheckCCompilerFlag)

set(WT_ARCH "riscv64" CACHE STRING "")
set(WT_OS "linux" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. Access to O_DIRECT on Linux. Append this macro to our compiler flags for Linux-based
# builds.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE" CACHE STRING "" FORCE)

# Linux requires buffers aligned to 4KB boundaries for O_DIRECT to work.
set(WT_BUFFER_ALIGNMENT_DEFAULT "4096" CACHE STRING "")

# See https://www.sifive.com/blog/all-aboard-part-1-compiler-args
# for background on the `rv64imafdc` and `lp64d` arguments here.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=rv64imafdc -mabi=lp64d" CACHE STRING "" FORCE)
