#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

include(cmake/strict/strict_flags_helpers.cmake)

# Get common GNU flags.
set(gcc_flags)
get_gnu_base_flags(gcc_flags C)

# FIX-ME-WT-8247: Add those flags to the common GNU flags if we want them for the compilation of the
# c++ files too.
list(APPEND gcc_flags "-Waggregate-return")
list(APPEND gcc_flags "-Wall")
list(APPEND gcc_flags "-Wextra")
list(APPEND gcc_flags "-Wshadow")
list(APPEND gcc_flags "-Wsign-conversion")

# Specific C flags:
list(APPEND gcc_flags "-Wbad-function-cast")
list(APPEND gcc_flags "-Wdeclaration-after-statement")
list(APPEND gcc_flags "-Wjump-misses-init")
list(APPEND gcc_flags "-Wmissing-prototypes")
list(APPEND gcc_flags "-Wnested-externs")
list(APPEND gcc_flags "-Wold-style-definition")
list(APPEND gcc_flags "-Wpointer-sign")
list(APPEND gcc_flags "-Wstrict-prototypes")

set(COMPILER_DIAGNOSTIC_C_FLAGS ${gcc_flags})
