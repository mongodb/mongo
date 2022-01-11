if(NOT HAVE_LIBTCMALLOC)
    # We don't need to construct a tcmalloc library target.
    return()
endif()

if(TARGET wt::tcmalloc)
    # Avoid redefining the imported library.
    return()
endif()

# Construct an imported tcmalloc target the project can use. We use the double colons (::) as
# a convention to tell CMake that the target name is associated with an IMPORTED target (which
# allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::tcmalloc SHARED IMPORTED GLOBAL)
set_target_properties(wt::tcmalloc PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBTCMALLOC}
)
if(HAVE_LIBTCMALLOC_INCLUDES)
    set_target_properties(wt::tcmalloc PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBTCMALLOC_INCLUDES}
    )
endif()
