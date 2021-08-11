#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

# We are not cross-compiling if our system is Darwin, hence the "x86_64-apple-darwin-"
# prefix is not necessary when we are not cross-compiling. Just default to the host
# installed 'gcc' binary.
if(CMAKE_CROSSCOMPILING)
    set(CROSS_COMPILER_PREFIX "x86_64-apple-darwin-")
endif()
