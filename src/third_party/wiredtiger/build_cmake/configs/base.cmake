#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

include(build_cmake/helpers.cmake)

# WiredTiger-related configuration options.

config_choice(
    WT_ARCH
    "Target architecture for WiredTiger"
    OPTIONS
        "x86;WT_X86;"
        "aarch64;WT_AARCH64;"
        "ppc64le;WT_PPC64;"
        "s390x;WT_S390X;"
)

config_choice(
    WT_OS
    "Target OS for WiredTiger"
    OPTIONS
        "darwin;WT_DARWIN;"
        "windows;WT_WIN;"
        "linux;WT_LINUX;"
)

config_bool(
    WT_POSIX
    "Is a posix platform"
    DEFAULT ON
    DEPENDS "WT_LINUX OR WT_DARWIN"
)

config_string(
    WT_BUFFER_ALIGNMENT_DEFAULT
    "WiredTiger buffer boundary aligment"
    DEFAULT 0
)

config_bool(
    HAVE_DIAGNOSTIC
    "Enable WiredTiger diagnostics"
    DEFAULT OFF
)

config_bool(
    HAVE_ATTACH
    "Enable to pause for debugger attach on failure"
    DEFAULT OFF
)

config_bool(
    ENABLE_STATIC
    "Compile as a static library"
    DEFAULT OFF
)

config_bool(
    ENABLE_STRICT
    "Compile with strict compiler warnings enabled"
    DEFAULT ON
)

config_bool(
    ENABLE_PYTHON
    "Configure the python API"
    DEFAULT OFF
    DEPENDS "NOT ENABLE_STATIC"
)

config_bool(
    WT_STANDALONE_BUILD
    "Support standalone build"
    DEFAULT ON
)

config_bool(
    HAVE_NO_CRC32_HARDWARE
    "Disable any crc32 hardware support"
    DEFAULT OFF
)

config_choice(
    SPINLOCK_TYPE
    "Set a spinlock type"
    OPTIONS
        "pthread;SPINLOCK_PTHREAD_MUTEX;HAVE_LIBPTHREAD"
        "gcc;SPINLOCK_GCC;"
        "msvc;SPINLOCK_MSVC;WT_WIN"
        "pthread_adaptive;SPINLOCK_PTHREAD_ADAPTIVE;HAVE_LIBPTHREAD"
)

config_bool(
    ENABLE_LZ4
    "Build the lz4 compressor extension"
    DEFAULT OFF
    DEPENDS "HAVE_LIBLZ4"
    # Specifically throw a fatal error if a user tries to enable the lz4 compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find lz4 library"
)

config_bool(
    ENABLE_SNAPPY
    "Build the snappy compressor extension"
    DEFAULT OFF
    DEPENDS "HAVE_LIBSNAPPY"
    # Specifically throw a fatal error if a user tries to enable the snappy compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find snappy library"
)

config_bool(
    ENABLE_ZLIB
    "Build the zlib compressor extension"
    DEFAULT OFF
    DEPENDS "HAVE_LIBZ"
    # Specifically throw a fatal error if a user tries to enable the zlib compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find zlib library"
)

config_bool(
    ENABLE_ZSTD
    "Build the libzstd compressor extension"
    DEFAULT OFF
    DEPENDS "HAVE_LIBZSTD"
    # Specifically throw a fatal error if a user tries to enable the zstd compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find zstd library"
)

config_bool(
    ENABLE_TCMALLOC
    "Use TCMalloc as the backend allocator"
    DEFAULT OFF
    DEPENDS "HAVE_LIBTCMALLOC"
    # Specifically throw a fatal error if a user tries to enable the tcmalloc allocator without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find tcmalloc library"
)

config_string(
    CC_OPTIMIZE_LEVEL
    "CC optimization level"
    DEFAULT "-O3"
)

config_string(
    VERSION_MAJOR
    "Major version number for WiredTiger"
    DEFAULT 10
)

config_string(
    VERSION_MINOR
    "Minor version number for WiredTiger"
    DEFAULT 0
)

config_string(
    VERSION_PATCH
    "Path version number for WiredTiger"
    DEFAULT 1
)


string(TIMESTAMP config_date "%Y-%m-%d")
config_string(
    VERSION_STRING
    "Version string for WiredTiger"
    DEFAULT "\"WiredTiger ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH} (${config_date})\""
)

if(HAVE_DIAGNOSTIC AND (NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
    # Avoid setting diagnostic flags if we are building with Debug mode.
    # CMakes Debug config sets compilation with debug symbols by default.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
endif()

if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    # Don't use the optimization level if we have specified a release config.
    # CMakes Release config sets compilation to the highest optimization level
    # by default.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CC_OPTIMIZE_LEVEL}")
endif()
