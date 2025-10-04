include(cmake/strict/strict_flags_helpers.cmake)

# Get common CL flags.
set(cl_flags)
get_cl_base_flags(cl_flags C)

# Specific C flags:

# Set our common compiler flags that can be used by the rest of our build.
set(COMPILER_DIAGNOSTIC_C_FLAGS ${cl_flags})
