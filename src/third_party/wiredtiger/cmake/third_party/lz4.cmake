if(NOT HAVE_LIBLZ4)
    # We don't need to construct a lz4 library target.
    return()
endif()

if(TARGET wt::lz4)
    # Avoid redefining the imported library, given this file can be used as an include.
    return()
endif()

# Define the imported lz4 library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::lz4 SHARED IMPORTED GLOBAL)
set_target_properties(wt::lz4 PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBLZ4}
    IMPORTED_IMPLIB ${HAVE_LIBLZ4}
)
if (HAVE_LIBLZ4_INCLUDES)
    set_target_properties(wt::lz4 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBLZ4_INCLUDES}
    )
endif()
