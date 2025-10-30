if(NOT ENABLE_PALITE)
    return() # PALite is disabled, skip the rest of this file
endif()

if(USE_SYSTEM_SQLITE3)
    find_package(SQLite3 ${SQLITE3_REQUIRED_VERSION} REQUIRED)
    add_library(wt::sqlite3 ALIAS SQLite::SQLite3)
    return()
endif()

# Use internal Sqlite3

message(CHECK_START "Looking for library SQLite3")

set(SQLITE3_DIR ${CMAKE_SOURCE_DIR}/test/3rdparty/sqlite3)

# Read the header file and find the SQLITE_VERSION definition
file(STRINGS "${SQLITE3_DIR}/sqlite3.h" SQLITE_VERSION_LINE
    REGEX "^#define[ \t]+SQLITE_VERSION[ \t]+\"[.0-9]+\"")
# Extract the version string (e.g., "3.XX.YY") from the matched line
string(REGEX MATCH "\"[.0-9]+\"" SQLITE3_VERSION "${SQLITE_VERSION_LINE}")
# Strip the quotes from the version string
string(REPLACE "\"" "" SQLITE3_VERSION "${SQLITE3_VERSION}")

if(${SQLITE3_VERSION} VERSION_LESS ${SQLITE3_REQUIRED_VERSION})
    message(FATAL_ERROR "Could NOT find SQLite3: Found unsuitable version"
        " \"${SQLITE3_VERSION}\", but required is at least"
        " \"${SQLITE3_REQUIRED_VERSION}\" (found ${SQLITE3_DIR})")
else()
    message(CHECK_PASS "Found internal SQLite3: ${SQLITE3_DIR}"
        " (found version \"${SQLITE3_VERSION}\")")
endif()

add_library(sqlite3_lib STATIC
    ${SQLITE3_DIR}/sqlite3.c
    ${SQLITE3_DIR}/sqlite3.h
)

target_include_directories(sqlite3_lib PUBLIC ${SQLITE3_DIR})

target_compile_definitions(sqlite3_lib PRIVATE
    # Omitting the possibility of using shared cache allows many conditionals in
    # performance-critical sections of the code to be eliminated. This can give
    # a noticeable improvement in performance.
    SQLITE_OMIT_SHARED_CACHE

    # Omit support for interfaces marked as deprecated.
    SQLITE_OMIT_DEPRECATED

    # The alloca() memory allocator will be used in a few situations where it is
    # appropriate. This can give a small performance improvement.
    SQLITE_USE_ALLOCA
)

set_target_properties(sqlite3_lib PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Disable all sanitizers for sqlite3_lib when sanitizer build types are used
string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)
if(CMAKE_BUILD_TYPE_UPPER MATCHES "^(ASAN|UBSAN|MSAN|TSAN)$")
    target_compile_options(sqlite3_lib PRIVATE
        $<$<NOT:$<BOOL:${MSVC_C_COMPILER}>>:-fno-sanitize=all>
    )
endif()

# Needed for SQLite3 on some platforms
target_link_libraries(sqlite3_lib PUBLIC
    $<$<BOOL:${WT_POSIX}>:${HAVE_LIBPTHREAD}>
    $<$<BOOL:${WT_POSIX}>:${HAVE_LIBDL}>
    $<$<BOOL:${WT_POSIX}>:m>
)

add_library(wt::sqlite3 ALIAS sqlite3_lib)

add_executable(sqlite3
    ${SQLITE3_DIR}/shell.c
)
target_link_libraries(sqlite3 PRIVATE sqlite3_lib)
