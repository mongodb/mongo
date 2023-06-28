include(ExternalProject)
include(GNUInstallDirs)
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)

# Skip the LazyFS build step if it is not enabled.
if(NOT ENABLE_LAZYFS)
    return()
endif()

if(TARGET lazyfs)
    # Avoid redefining the imported library, given this file can be used as an include.
    return()
endif()

# Download and install the project into the build directory.
ExternalProject_Add(lazyfs
    PREFIX lazyfs
    GIT_CONFIG advice.detachedHead=false 
    GIT_REPOSITORY https://github.com/dsrhaslab/lazyfs.git
    GIT_TAG b0383127
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND cd libs/libpcache && ./build.sh COMMAND cd ../../lazyfs && ./build.sh
    INSTALL_COMMAND ""
)
