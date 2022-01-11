include(cmake/strict/strict_flags_helpers.cmake)

# Get common CLANG flags.
set(clangxx_flags)
get_clang_base_flags(clangxx_flags CXX)

# Specific CXX flags:

# Set our common compiler flags that can be used by the rest of our build.
set(COMPILER_DIAGNOSTIC_CXX_FLAGS ${clangxx_flags})
