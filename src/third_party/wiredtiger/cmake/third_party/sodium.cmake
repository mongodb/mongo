if(NOT HAVE_LIBSODIUM)
    # We don't need to construct a sodium library target.
    return()
endif()

if(TARGET wt::sodium)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported sodium library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::sodium SHARED IMPORTED GLOBAL)
set_target_properties(wt::sodium PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBSODIUM}
)
if (HAVE_LIBSODIUM_INCLUDES)
    set_target_properties(wt::sodium PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBSODIUM_INCLUDES}
    )
endif()
