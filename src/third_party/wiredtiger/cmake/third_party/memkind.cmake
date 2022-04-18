if(NOT HAVE_LIBMEMKIND)
    # We can't construct a memkind library target.
    return()
endif()

if (NOT ENABLE_MEMKIND)
  # We don't want to construct a memkind library target.
  return()
endif()

if(TARGET wt::memkind)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported memkind library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::memkind SHARED IMPORTED GLOBAL)
set_target_properties(wt::memkind PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBMEMKIND}
)
if (HAVE_LIBMEMKIND_INCLUDES)
    set_target_properties(wt::memkind PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBMEMKIND_INCLUDES}
    )
endif()
