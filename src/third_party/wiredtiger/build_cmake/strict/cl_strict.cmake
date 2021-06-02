#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

# Warning level 3.
list(APPEND win_c_flags "/WX")
# Ignore warning about mismatched const qualifiers.
list(APPEND win_c_flags "/wd4090")
# Ignore deprecated functions.
list(APPEND win_c_flags "/wd4996")
# Complain about unreferenced format parameter.
list(APPEND win_c_flags "/we4100")
# Enable security check.
list(APPEND win_c_flags "/GS")

# Set our base compiler flags that can be used by the rest of our build.
set(COMPILER_DIAGNOSTIC_FLAGS "${COMPILER_DIAGNOSTIC_FLAGS};${win_c_flags}" CACHE INTERNAL "" FORCE)
