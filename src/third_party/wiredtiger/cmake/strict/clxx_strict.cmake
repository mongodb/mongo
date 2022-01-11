include(cmake/strict/strict_flags_helpers.cmake)

# Get common CL flags.
set(clxx_flags)
get_cl_base_flags(clxx_flags CXX)

# Specific CXX flags:

# Set our common compiler flags that can be used by the rest of our build.
set(COMPILER_DIAGNOSTIC_CXX_FLAGS ${clxx_flags})
