include(cmake/strict/strict_flags_helpers.cmake)

# Get common GNU flags.
set(gxx_flags)
get_gnu_base_flags(gxx_flags CXX)

# Specific CXX flags:

set(COMPILER_DIAGNOSTIC_CXX_FLAGS ${gxx_flags})
