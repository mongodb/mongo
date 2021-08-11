#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
# See the file LICENSE for redistribution information.
#

list(APPEND gcc_base_c_flags "-Wall")
list(APPEND gcc_base_c_flags "-Wextra")
list(APPEND gcc_base_c_flags "-Werror")
list(APPEND gcc_base_c_flags "-Waggregate-return")
list(APPEND gcc_base_c_flags "-Wbad-function-cast")
list(APPEND gcc_base_c_flags "-Wcast-align")
list(APPEND gcc_base_c_flags "-Wdeclaration-after-statement")
list(APPEND gcc_base_c_flags "-Wdouble-promotion")
list(APPEND gcc_base_c_flags "-Wfloat-equal")
list(APPEND gcc_base_c_flags "-Wformat-nonliteral")
list(APPEND gcc_base_c_flags "-Wformat-security")
list(APPEND gcc_base_c_flags "-Wformat=2")
list(APPEND gcc_base_c_flags "-Winit-self")
list(APPEND gcc_base_c_flags "-Wjump-misses-init")
list(APPEND gcc_base_c_flags "-Wmissing-declarations")
list(APPEND gcc_base_c_flags "-Wmissing-field-initializers")
list(APPEND gcc_base_c_flags "-Wmissing-prototypes")
list(APPEND gcc_base_c_flags "-Wnested-externs")
list(APPEND gcc_base_c_flags "-Wold-style-definition")
list(APPEND gcc_base_c_flags "-Wpacked")
list(APPEND gcc_base_c_flags "-Wpointer-arith")
list(APPEND gcc_base_c_flags "-Wpointer-sign")
list(APPEND gcc_base_c_flags "-Wredundant-decls")
list(APPEND gcc_base_c_flags "-Wshadow")
list(APPEND gcc_base_c_flags "-Wsign-conversion")
list(APPEND gcc_base_c_flags "-Wstrict-prototypes")
list(APPEND gcc_base_c_flags "-Wswitch-enum")
list(APPEND gcc_base_c_flags "-Wundef")
list(APPEND gcc_base_c_flags "-Wuninitialized")
list(APPEND gcc_base_c_flags "-Wunreachable-code")
list(APPEND gcc_base_c_flags "-Wunused")
list(APPEND gcc_base_c_flags "-Wwrite-strings")

# Non-fatal informational warnings.
# We don't turn on the unsafe-loop-optimizations warning after gcc7,
# it's too noisy to tolerate. Regardless, don't fail even when it's
# configured.
list(APPEND gcc_base_c_flags "-Wno-error=unsafe-loop-optimizations")
if(${CMAKE_C_COMPILER_VERSION} VERSION_EQUAL 4.7)
    list(APPEND gcc_base_c_flags "-Wno-c11-extensions")
    list(APPEND gcc_base_c_flags "-Wunsafe-loop-optimizations")
elseif(${CMAKE_C_COMPILER_VERSION} VERSION_EQUAL 5)
    list(APPEND gcc_base_c_flags "-Wunsafe-loop-optimizations")
endif()

if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 5)
    list(APPEND gcc_base_c_flags "-Wformat-signedness")
    list(APPEND gcc_base_c_flags "-Wjump-misses-init")
    list(APPEND gcc_base_c_flags "-Wredundant-decls")
    list(APPEND gcc_base_c_flags "-Wunused-macros")
    list(APPEND gcc_base_c_flags "-Wvariadic-macros")
endif()
if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 6)
    list(APPEND gcc_base_c_flags "-Wduplicated-cond")
    list(APPEND gcc_base_c_flags "-Wlogical-op")
    list(APPEND gcc_base_c_flags "-Wunused-const-variable=2")
endif()
if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 7)
    list(APPEND gcc_base_c_flags "-Walloca")
    list(APPEND gcc_base_c_flags "-Walloc-zero")
    list(APPEND gcc_base_c_flags "-Wduplicated-branches")
    list(APPEND gcc_base_c_flags "-Wformat-overflow=2")
    list(APPEND gcc_base_c_flags "-Wformat-truncation=2")
    list(APPEND gcc_base_c_flags "-Wrestrict")
endif()
if(${CMAKE_C_COMPILER_VERSION} VERSION_GREATER_EQUAL 8)
    list(APPEND gcc_base_c_flags "-Wmultistatement-macros")
endif()

# Set our base gcc flags to ensure it propogates to the rest of our build.
set(COMPILER_DIAGNOSTIC_FLAGS "${COMPILER_DIAGNOSTIC_FLAGS};${gcc_base_c_flags}" CACHE INTERNAL "" FORCE)
