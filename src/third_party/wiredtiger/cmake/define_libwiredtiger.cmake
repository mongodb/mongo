#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

# define_wiredtiger_library(target type)
# A helper that defines a wiredtiger library target. This defining a set of common targets and properties we
# want to be associated to any given 'libwiredtiger' target. Having this as a macro allows us to de-duplicate common
# definitions when creating multiple versions of libwiredtiger i.e. static and shared builds. Note: this
# macro assumes you only call it once per libwiredtiger flavour (e.g. only one 'static' definition), use carefully.
#   target - Name of the libwiredtiger target.
#   type - Library type of wiredtiger target e.g. STATIC, SHARED or MODULE.
#   SOURCES - Sources to be compiled under the wiredtiger library. Requires that at least one source file/object is defined.
#   PUBLIC_INCLUDES - Public interface includes of the wiredtiger library.
#   PRIVATE_INCLUDES - Private interface includes of the wiredtiger library.
macro(define_wiredtiger_library target type)
    cmake_parse_arguments(
        "DEFINE_WT"
        ""
        ""
        "SOURCES;PUBLIC_INCLUDES;PRIVATE_INCLUDES"
        ${ARGN}
    )
    if (NOT "${DEFINE_WT_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "Unknown arguments to define_wiredtiger_library: ${DEFINE_WT_UNPARSED_ARGUMENTS}")
    endif()
    if ("${DEFINE_WT_SOURCES}" STREQUAL "")
        message(FATAL_ERROR "No sources given to define_wiredtiger_library")
    endif()

    # Define the wiredtiger library target.
    add_library(${target} ${type} ${DEFINE_WT_SOURCES})
    # Append any include directories to the library target.
    if(DEFINE_WT_PUBLIC_INCLUDES)
        target_include_directories(${target} PUBLIC ${DEFINE_WT_PUBLIC_INCLUDES})
    endif()
    if(DEFINE_WT_PRIVATE_INCLUDES)
        target_include_directories(${target} PRIVATE ${DEFINE_WT_PRIVATE_INCLUDES})
    endif()
    # Append any provided C flags.
    if(COMPILER_DIAGNOSTIC_C_FLAGS)
        target_compile_options(${target} PRIVATE ${COMPILER_DIAGNOSTIC_C_FLAGS})
    endif()

    # We want to set the following target properties:
    # OUTPUT_NAME - Generate a library with the name "libwiredtiger[.so|.a". Note this assumes each invocation
    #   of this macro is specifying a unique libwiredtiger target type (e.g 'SHARED', 'STATIC'), multiple declarations
    #   of a 'SHARED' wiredtiger library would conflict.
    # NO_SYSTEM_FROM_IMPORTED - don't treat include interface directories consumed on an imported target as system
    #   directories.
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "wiredtiger"
        NO_SYSTEM_FROM_IMPORTED TRUE
    )

    # Ensure we link any available library dependencies to our wiredtiger target.
    if(HAVE_LIBPTHREAD)
        target_link_libraries(${target} PUBLIC ${HAVE_LIBPTHREAD})
        if(HAVE_LIBPTHREAD_INCLUDES)
            target_include_directories(${target} PUBLIC ${HAVE_LIBPTHREAD_INCLUDES})
        endif()
    endif()
    if(HAVE_LIBRT)
        target_link_libraries(${target} PUBLIC ${HAVE_LIBRT})
        if(HAVE_LIBRT_INCLUDES)
            target_include_directories(${target} PUBLIC ${HAVE_LIBRT_INCLUDES})
        endif()
    endif()
    if(HAVE_LIBDL)
        target_link_libraries(${target} PUBLIC ${HAVE_LIBDL})
        if(HAVE_LIBDL_INCLUDES)
            target_include_directories(${target} PUBLIC ${HAVE_LIBDL_INCLUDES})
        endif()
    endif()
    if(ENABLE_TCMALLOC)
        target_link_libraries(${target} PRIVATE wt::tcmalloc)
    endif()

    # We want to capture any transitive dependencies associated with the builtin library
    # target and ensure we are explicitly linking the 3rd party libraries.
    if(HAVE_BUILTIN_EXTENSION_LZ4)
        target_link_libraries(${target} PRIVATE wt::lz4)
    endif()

    if(HAVE_BUILTIN_EXTENSION_SNAPPY)
        target_link_libraries(${target} PRIVATE wt::snappy)
    endif()

    if(HAVE_BUILTIN_EXTENSION_SODIUM)
        target_link_libraries(${target} PRIVATE wt::sodium)
    endif()

    if(HAVE_BUILTIN_EXTENSION_ZLIB)
        target_link_libraries(${target} PRIVATE wt::zlib)
    endif()

    if(HAVE_BUILTIN_EXTENSION_ZSTD)
        target_link_libraries(${target} PRIVATE wt::zstd)
    endif()
endmacro()
