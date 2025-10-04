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
list(APPEND gcc_flags "-Wno-declaration-after-statement")
list(APPEND gcc_flags "-Wjump-misses-init")
list(APPEND gcc_flags "-Wmissing-prototypes")
list(APPEND gcc_flags "-Wnested-externs")
list(APPEND gcc_flags "-Wold-style-definition")
list(APPEND gcc_flags "-Wpointer-sign")
list(APPEND gcc_flags "-Wstrict-prototypes")

# We only turn on the unsafe-loop-optimizations warning before gcc7,
# it's too noisy to tolerate otherwise.
if(${CMAKE_C_COMPILER_VERSION} VERSION_EQUAL 4.7)
    list(APPEND gcc_flags "-Wunsafe-loop-optimizations")
elseif(${CMAKE_C_COMPILER_VERSION} VERSION_EQUAL 5)
    list(APPEND gcc_flags "-Wunsafe-loop-optimizations")
elseif(${CMAKE_C_COMPILER_VERSION} VERSION_EQUAL 6)
    list(APPEND gcc_flags "-Wunsafe-loop-optimizations")
endif()

# In code coverage builds inline functions may not be inlined, which can result in additional
# unused copies of those functions, so the unused-function warning much be turned off.
if(CODE_COVERAGE_MEASUREMENT)
    list(APPEND gcc_flags "-Wno-unused-function")
endif ()

set(COMPILER_DIAGNOSTIC_C_FLAGS ${gcc_flags})
