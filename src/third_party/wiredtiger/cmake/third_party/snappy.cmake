if(NOT HAVE_LIBSNAPPY)
    # We don't need to construct a snappy library target.
    return()
endif()

if(TARGET wt::snappy)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported snappy library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::snappy SHARED IMPORTED GLOBAL)
set_target_properties(wt::snappy PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBSNAPPY}
)
if (HAVE_LIBSNAPPY_INCLUDES)
    set_target_properties(wt::snappy PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBSNAPPY_INCLUDES}
    )
endif()
