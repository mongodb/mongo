# get_gnu_base_flags(flags)
# Helper function that generates a set of common GNU flags for a given language.
#   flags - list of flags.
function(get_gnu_base_flags flags)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "GNU_FLAGS"
        "CXX;C"
        ""
        ""
    )

    if (NOT "${GNU_FLAGS_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to get_gnu_base_flags: ${GNU_FLAGS_UNPARSED_ARGUMENTS}")
    endif()

    set(cmake_compiler_version)

    if(${GNU_FLAGS_C} AND ${GNU_FLAGS_CXX})
        message(FATAL_ERROR "Only one language is accepted")
    elseif(GNU_FLAGS_C)
        set(cmake_compiler_version ${CMAKE_C_COMPILER_VERSION})
    elseif(GNU_FLAGS_CXX)
        set(cmake_compiler_version ${CMAKE_CXX_COMPILER_VERSION})
    else()
        message(FATAL_ERROR "No language passed")
    endif()

    set(gnu_flags)

    list(APPEND gnu_flags "-Wcast-align")
    list(APPEND gnu_flags "-Wdouble-promotion")
    list(APPEND gnu_flags "-Werror")
    list(APPEND gnu_flags "-Wfloat-equal")
    list(APPEND gnu_flags "-Wformat-nonliteral")
    list(APPEND gnu_flags "-Wformat-security")
    list(APPEND gnu_flags "-Wformat=2")
    list(APPEND gnu_flags "-Winit-self")
    list(APPEND gnu_flags "-Wmissing-declarations")
    list(APPEND gnu_flags "-Wmissing-field-initializers")
    list(APPEND gnu_flags "-Wpacked")
    list(APPEND gnu_flags "-Wpointer-arith")
    list(APPEND gnu_flags "-Wredundant-decls")
    list(APPEND gnu_flags "-Wswitch-enum")
    list(APPEND gnu_flags "-Wundef")
    list(APPEND gnu_flags "-Wuninitialized")
    list(APPEND gnu_flags "-Wunreachable-code")
    list(APPEND gnu_flags "-Wunused")
    list(APPEND gnu_flags "-Wwrite-strings")

    # Non-fatal informational warnings.
    # The unsafe-loop-optimizations warning is only enabled for specific gcc versions.
    # Regardless, don't fail when it's configured.
    list(APPEND gnu_flags "-Wno-error=unsafe-loop-optimizations")

    if(${cmake_compiler_version} VERSION_EQUAL 4.7)
        list(APPEND gnu_flags "-Wno-c11-extensions")
    endif()
    if(${cmake_compiler_version} VERSION_GREATER_EQUAL 5)
        list(APPEND gnu_flags "-Wformat-signedness")
        list(APPEND gnu_flags "-Wunused-macros")
        list(APPEND gnu_flags "-Wvariadic-macros")
    endif()
    if(${cmake_compiler_version} VERSION_GREATER_EQUAL 6)
        list(APPEND gnu_flags "-Wduplicated-cond")
        list(APPEND gnu_flags "-Wlogical-op")
        list(APPEND gnu_flags "-Wunused-const-variable=2")
    endif()
    if(${cmake_compiler_version} VERSION_GREATER_EQUAL 7)
        list(APPEND gnu_flags "-Walloca")
        list(APPEND gnu_flags "-Walloc-zero")
        list(APPEND gnu_flags "-Wduplicated-branches")
        list(APPEND gnu_flags "-Wformat-overflow=2")
        list(APPEND gnu_flags "-Wformat-truncation=2")
        list(APPEND gnu_flags "-Wrestrict")
    endif()
    if(${cmake_compiler_version} VERSION_GREATER_EQUAL 8)
        list(APPEND gnu_flags "-Wmultistatement-macros")
    endif()

    set(${flags} ${gnu_flags} PARENT_SCOPE)

endfunction()

# get_clang_base_flags(flags)
# Helper function that generates a set of common CLANG flags for a given language.
#   flags - list of flags.
function(get_clang_base_flags flags)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "CLANG_FLAGS"
        "CXX;C"
        ""
        ""
    )

    if (NOT "${CLANG_FLAGS_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to get_clang_base_flags: ${CLANG_FLAGS_UNPARSED_ARGUMENTS}")
    endif()

    set(cmake_compiler_version)

    if(${CLANG_FLAGS_C} AND ${CLANG_FLAGS_CXX})
        message(FATAL_ERROR "Only one language is accepted")
    elseif(CLANG_FLAGS_C)
        set(cmake_compiler_version ${CMAKE_C_COMPILER_VERSION})
    elseif(CLANG_FLAGS_CXX)
        set(cmake_compiler_version ${CMAKE_CXX_COMPILER_VERSION})
    else()
        message(FATAL_ERROR "No language passed")
    endif()

    set(clang_flags)

    list(APPEND clang_flags "-Werror")
    list(APPEND clang_flags "-Wno-cast-align")
    list(APPEND clang_flags "-Wno-documentation-unknown-command")
    list(APPEND clang_flags "-Wno-format-nonliteral")
    list(APPEND clang_flags "-Wno-packed")
    list(APPEND clang_flags "-Wno-padded")
    list(APPEND clang_flags "-Wno-reserved-id-macro")
    list(APPEND clang_flags "-Wno-zero-length-array")

    # We should turn on cast-qual, but not as a fatal error: see WT-2690.
    # For now, turn it off.
    list(APPEND clang_flags "-Wno-cast-qual")

    # Turn off clang thread-safety-analysis, it doesn't like some of the
    # code patterns in WiredTiger.
    list(APPEND clang_flags "-Wno-thread-safety-analysis")

    # On Centos 7.3.1611, system header files aren't compatible with
    # -Wdisabled-macro-expansion.
    list(APPEND clang_flags "-Wno-disabled-macro-expansion")

    # We occasionally use an extra semicolon to indicate an empty loop or
    # conditional body.
    list(APPEND clang_flags "-Wno-extra-semi-stmt")

    # Ignore unrecognized options.
    list(APPEND clang_flags "-Wno-unknown-warning-option")

    if(WT_DARWIN AND NOT CMAKE_CROSSCOMPILING)
        # If we are not cross-compiling, we can safely disable this diagnostic.
        # Its incompatible with strict diagnostics when including external
        # libraries that are not in the default linker path
        # e.g. linking zlib/snappy/... from /usr/local/.
        list(APPEND clang_flags "-Wno-poison-system-directories")
    endif()

    if(WT_DARWIN AND (${cmake_compiler_version} VERSION_EQUAL 4.1))
        # Apple clang has its own numbering system, and older OS X
        # releases need some special love. Turn off some flags for
        # Apple's clang 4.1:
        #   Apple clang version 4.1
        #   ((tags/Apple/clang-421.11.66) (based on LLVM 3.1svn)
        list(APPEND clang_flags "-Wno-attributes")
        list(APPEND clang_flags "-Wno-pedantic")
        list(APPEND clang_flags "-Wno-unused-command-line-argument")
    endif()

    # FIXME-WT-8052: Figure out whether we want to disable these or change the code.
    if(${cmake_compiler_version} VERSION_GREATER_EQUAL 10)
        # Clang 10+ has added additional on-by-default diagnostics that isn't
        # compatible with some of the code patterns in WiredTiger.
        list(APPEND clang_flags "-Wno-implicit-fallthrough")
        list(APPEND clang_flags "-Wno-implicit-int-float-conversion")
    endif()

    set(${flags} ${clang_flags} PARENT_SCOPE)

endfunction()

# get_cl_base_flags(flags)
# Helper function that generates a set of common CL flags for a given language.
#   flags - list of flags.
function(get_cl_base_flags flags)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        "CL_FLAGS"
        "CXX;C"
        ""
        ""
    )

    if (NOT "${CL_FLAGS_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to get_cl_base_flags: ${CL_FLAGS_UNPARSED_ARGUMENTS}")
    endif()

    set(cmake_compiler_version)

    if(${CL_FLAGS_C} AND ${CL_FLAGS_CXX})
        message(FATAL_ERROR "Only one language is accepted")
    elseif(CL_FLAGS_C)
        set(cmake_compiler_version ${CMAKE_C_COMPILER_VERSION})
    elseif(CL_FLAGS_CXX)
        set(cmake_compiler_version ${CMAKE_CXX_COMPILER_VERSION})
    else()
        message(FATAL_ERROR "No language passed")
    endif()

    set(cl_flags)

    # Warning level 3.
    list(APPEND cl_flags "/WX")
    # Ignore warning about mismatched const qualifiers.
    list(APPEND cl_flags "/wd4090")
    # Ignore deprecated functions.
    list(APPEND cl_flags "/wd4996")
    # Complain about unreferenced format parameter.
    list(APPEND cl_flags "/we4100")
    # Enable security check.
    list(APPEND cl_flags "/GS")

    set(${flags} ${cl_flags} PARENT_SCOPE)

endfunction()
