if(NOT HAVE_LIBZSTD)
    # We don't need to construct a zstd library target.
    return()
endif()

if(TARGET wt::zstd)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported zstd library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::zstd SHARED IMPORTED GLOBAL)
set_target_properties(wt::zstd PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBZSTD}
)
if (HAVE_LIBZSTD_INCLUDES)
    set_target_properties(wt::zstd PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBZSTD_INCLUDES}
    )
endif()
