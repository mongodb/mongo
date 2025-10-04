include(cmake/strict/strict_flags_helpers.cmake)

# Get common GNU flags.
set(gxx_flags)
get_gnu_base_flags(gxx_flags CXX)

# Specific CXX flags:

# In code coverage builds inline functions may not be inlined, which can result in additional
# unused copies of those functions, so the unused-function warning much be turned off.
if(CODE_COVERAGE_MEASUREMENT)
    list(APPEND gxx_flags "-Wno-unused-function")
endif ()

set(COMPILER_DIAGNOSTIC_CXX_FLAGS ${gxx_flags})
