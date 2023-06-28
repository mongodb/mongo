if(TARGET wt::voidstar)
    # Avoid redefining the imported library.
    return()
endif()

# Define the imported voidstar library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::voidstar INTERFACE IMPORTED GLOBAL)
set_target_properties(wt::voidstar PROPERTIES
    IMPORTED_LIBNAME voidstar
)

link_directories(${CMAKE_SOURCE_DIR}/tools/voidstar/lib)

set_target_properties(wt::voidstar PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/tools/voidstar/include
)
