if(NOT HAVE_LIBQPL)
    # We don't need to construct a iaa library target.
    return()
endif()

if(TARGET wt::qpl)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported iaa library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::qpl STATIC IMPORTED GLOBAL)
set_target_properties(wt::qpl PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBQPL}
)
if (HAVE_LIBQPL_INCLUDES)
    set_target_properties(wt::qpl PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBQPL_INCLUDES}
    )
endif()
