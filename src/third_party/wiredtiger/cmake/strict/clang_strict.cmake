include(cmake/strict/strict_flags_helpers.cmake)

# Get common CLANG flags.
set(clang_flags)
get_clang_base_flags(clang_flags C)

# Specific C flags:
list(APPEND clang_flags "-Weverything")

# Set our common compiler flags that can be used by the rest of our build.
set(COMPILER_DIAGNOSTIC_C_FLAGS ${clang_flags})
