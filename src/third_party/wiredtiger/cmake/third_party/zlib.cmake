if(NOT HAVE_LIBZ)
    # We don't need to construct a zlib library target.
    return()
endif()

if(TARGET wt::zlib)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported zlib library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::zlib SHARED IMPORTED GLOBAL)
set_target_properties(wt::zlib PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBZ}
    IMPORTED_IMPLIB ${HAVE_LIBZ}
)
if (HAVE_LIBZ_INCLUDES)
    set_target_properties(wt::zlib PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBZ_INCLUDES}
    )
endif()
