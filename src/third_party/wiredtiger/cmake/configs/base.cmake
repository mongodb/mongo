include(cmake/helpers.cmake)
include(cmake/configs/version.cmake)

# WiredTiger-related configuration options.

config_choice(
    WT_ARCH
    "Target architecture for WiredTiger"
    OPTIONS
        "x86;WT_X86;"
        "aarch64;WT_AARCH64;"
        "ppc64le;WT_PPC64;"
        "s390x;WT_S390X;"
        "riscv64;WT_RISCV64;"
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
    "WiredTiger buffer boundary alignment"
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
    ENABLE_SHARED
    "Compile as a shared library"
    DEFAULT ON
)

config_bool(
    WITH_PIC
    "Generate position-independent code. Note PIC will always \
    be used on shared targets, irrespective of the value of this configuration."
    DEFAULT OFF
)

config_bool(
    ENABLE_STRICT
    "Compile with strict compiler warnings enabled"
    DEFAULT OFF
)

config_bool(
    ENABLE_PYTHON
    "Configure the python API"
    DEFAULT OFF
)

config_string(
    PYTHON3_REQUIRED_VERSION
    "Exact Python version to use when building the Python API. \
    By default, when this configuration is unset, CMake will preference the \
    highest python version found to be installed in the users system path. \
    Expected format of version string: major[.minor[.patch]]"
    DEFAULT ""
    DEPENDS "ENABLE_PYTHON"
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

config_bool(
    DYNAMIC_CRT
    "Link with the MSVCRT DLL version"
    DEFAULT OFF
    DEPENDS "WT_WIN"
)

config_choice(
    SPINLOCK_TYPE
    "Set a spinlock type"
    OPTIONS
        "pthread;SPINLOCK_PTHREAD_MUTEX;HAVE_LIBPTHREAD"
        "gcc;SPINLOCK_GCC;"
        "msvc;SPINLOCK_MSVC;WT_WIN"
        "pthread_adaptive;SPINLOCK_PTHREAD_MUTEX_ADAPTIVE;HAVE_LIBPTHREAD"
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
    ENABLE_SODIUM
    "Build the libsodium encryption extension"
    DEFAULT OFF
    DEPENDS "HAVE_LIBSODIUM"
    # Specifically throw a fatal error if a user tries to enable the libsodium encryptor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find sodium library"
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

config_bool(
    ENABLE_S3
    "Build the S3 storage extension"
    DEFAULT OFF
)

set(default_optimize_level)
if("${WT_OS}" STREQUAL "windows")
    set(default_optimize_level "/O2")
else()
    set(default_optimize_level "-O3")
endif()
config_string(
    CC_OPTIMIZE_LEVEL
    "CC optimization level"
    DEFAULT "${default_optimize_level}"
)

config_string(
    VERSION_MAJOR
    "Major version number for WiredTiger"
    DEFAULT ${WT_VERSION_MAJOR}
)

config_string(
    VERSION_MINOR
    "Minor version number for WiredTiger"
    DEFAULT ${WT_VERSION_MINOR}
)

config_string(
    VERSION_PATCH
    "Path version number for WiredTiger"
    DEFAULT ${WT_VERSION_PATCH}
)

config_string(
    VERSION_STRING
    "Version string for WiredTiger"
    DEFAULT "\"${WT_VERSION_STRING}\""
)

if(HAVE_DIAGNOSTIC AND (NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
    # Avoid setting diagnostic flags if we are building with Debug mode.
    # CMakes Debug config sets compilation with debug symbols by default.
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # Produce full symbolic debugging information.
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Z7")
        # Ensure a PDB file can be generated for debugging symbols.
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
    endif()
endif()

if(WT_WIN)
    # Check if we a using the dynamic or static run-time library.
    if(DYNAMIC_CRT)
        # Use the multithread-specific and DLL-specific version of the run-time library (MSVCRT.lib).
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MD")
    else()
        # Use the multithread, static version of the run-time library.
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MT")
    endif()
endif()

if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    # Don't use the optimization level if we have specified a release config.
    # CMakes Release config sets compilation to the highest optimization level
    # by default.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CC_OPTIMIZE_LEVEL}")
endif()
