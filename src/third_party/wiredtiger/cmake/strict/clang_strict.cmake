include(cmake/strict/strict_flags_helpers.cmake)

# Get common CLANG flags.
set(clang_flags)
get_clang_base_flags(clang_flags C)

# Specific C flags:
list(APPEND clang_flags "-Weverything")
list(APPEND clang_flags "-Wno-declaration-after-statement")
list(APPEND clang_flags "-Wjump-misses-init")
list(APPEND clang_flags "-Wconditional-uninitialized")
list(APPEND clang_flags "-Wno-pre-c11-compat")
list(APPEND clang_flags "-Wno-switch-default")

# In code coverage builds inline functions may not be inlined, which can result in additional
# unused copies of those functions, so the unused-function warning much be turned off.
if(CODE_COVERAGE_MEASUREMENT)
    list(APPEND clang_flags "-Wno-unused-function")
endif ()

# Set our common compiler flags that can be used by the rest of our build.
set(COMPILER_DIAGNOSTIC_C_FLAGS ${clang_flags})
