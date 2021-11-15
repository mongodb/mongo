#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

include(cmake/strict/strict_flags_helpers.cmake)

# Get common GNU flags.
set(gxx_flags)
get_gnu_base_flags(gxx_flags CXX)

# Specific CXX flags:

set(COMPILER_DIAGNOSTIC_CXX_FLAGS ${gxx_flags})
