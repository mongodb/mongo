# Find LibLZ4
#
# Find LibLZ4 headers and library
#
#   Result Variables
#
#   LIBLZ4_FOUND             - True if lz4 is found
#   LIBLZ4_INCLUDE_DIRS      - lz4 headers directories
#   LIBLZ4_LIBRARIES         - lz4 libraries
#   LIBLZ4_VERSION_MAJOR     - The major version of lz4
#   LIBLZ4_VERSION_MINOR     - The minor version of lz4
#   LIBLZ4_VERSION_RELEASE   - The release version of lz4
#   LIBLZ4_VERSION_STRING    - version number string (e.g. 1.8.3)
#
#   Hints
#
#   Set ``LZ4_ROOT_DIR`` to the directory of lz4.h and lz4 library

set(_LIBLZ4_ROOT_HINTS
    ENV LZ4_ROOT_DIR)

find_path(  LIBLZ4_INCLUDE_DIR lz4.h
            HINTS ${_LIBLZ4_ROOT_HINTS})
find_library(   LIBLZ4_LIBRARY NAMES lz4 liblz4 liblz4_static
                HINTS ${_LIBLZ4_ROOT_HINTS})

if(LIBLZ4_INCLUDE_DIR)
    file(STRINGS "${LIBLZ4_INCLUDE_DIR}/lz4.h" LIBLZ4_HEADER_CONTENT REGEX "#define LZ4_VERSION_[A-Z]+ +[0-9]+")

    string(REGEX REPLACE ".*#define LZ4_VERSION_MAJOR +([0-9]+).*" "\\1" LIBLZ4_VERSION_MAJOR "${LIBLZ4_HEADER_CONTENT}")
    string(REGEX REPLACE ".*#define LZ4_VERSION_MINOR +([0-9]+).*" "\\1" LIBLZ4_VERSION_MINOR "${LIBLZ4_HEADER_CONTENT}")
    string(REGEX REPLACE ".*#define LZ4_VERSION_RELEASE +([0-9]+).*" "\\1" LIBLZ4_VERSION_RELEASE "${LIBLZ4_HEADER_CONTENT}")

    set(LIBLZ4_VERSION_STRING "${LIBLZ4_VERSION_MAJOR}.${LIBLZ4_VERSION_MINOR}.${LIBLZ4_VERSION_RELEASE}")
    unset(LIBLZ4_HEADER_CONTENT)
endif()

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LibLZ4    REQUIRED_VARS   LIBLZ4_INCLUDE_DIR
                                                            LIBLZ4_LIBRARY
                                            VERSION_VAR     LIBLZ4_VERSION_STRING
                                            FAIL_MESSAGE    "Could NOT find LZ4, try to set the paths to lz4.h and lz4 library in environment variable LZ4_ROOT_DIR")

if (LIBLZ4_FOUND)
    set(LIBLZ4_LIBRARIES ${LIBLZ4_LIBRARY})
    set(LIBLZ4_INCLUDE_DIRS ${LIBLZ4_INCLUDE_DIR})
endif ()

mark_as_advanced( LIBLZ4_INCLUDE_DIR LIBLZ4_LIBRARY )
