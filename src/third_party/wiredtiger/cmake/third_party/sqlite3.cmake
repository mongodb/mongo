if(NOT ENABLE_INTERNAL_SQLITE3)
    find_package(SQLite3 ${SQLITE3_REQUIRED_VERSION} REQUIRED)
    add_library(wt::sqlite3 ALIAS SQLite::SQLite3)
    return()
endif()

# Use internal Sqlite3

message(CHECK_START "Looking for library SQLite3")

set(SQLITE3_DIR ${CMAKE_SOURCE_DIR}/test/3rdparty/sqlite3)

add_library(sqlite3_lib STATIC
    ${SQLITE3_DIR}/sqlite3.c
    ${SQLITE3_DIR}/sqlite3.h
)
target_include_directories(sqlite3_lib PUBLIC ${SQLITE3_DIR})
set_target_properties(sqlite3_lib PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

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

# Read the header file and find the SQLITE_VERSION definition
file(STRINGS "${SQLITE3_DIR}/sqlite3.h" SQLITE_VERSION_LINE
    REGEX "^#define[ \t]+SQLITE_VERSION[ \t]+\"[.0-9]+\"")
# Extract the version string (e.g., "3.XX.YY") from the matched line
string(REGEX MATCH "\"[.0-9]+\"" SQLITE3_VERSION "${SQLITE_VERSION_LINE}")

message(CHECK_PASS "Found internal SQLite3: ${SQLITE3_DIR}"
    " (found version ${SQLITE3_VERSION})")
