include(cmake/helpers.cmake)
include(cmake/configs/version.cmake)

# Setup defaults based on the build type and available libraries.
set(default_have_diagnostics ON)
set(default_enable_python OFF)
set(default_enable_lz4 OFF)
set(default_enable_snappy OFF)
set(default_enable_zlib OFF)
set(default_enable_zstd OFF)
set(default_enable_tcmalloc ${HAVE_LIBTCMALLOC})
set(default_enable_debug_info ON)
set(default_enable_static OFF)
set(default_enable_shared ON)

if("${CMAKE_BUILD_TYPE}" MATCHES "^(Release|RelWithDebInfo)$")
    set(default_have_diagnostics OFF)
endif()

# Enable python if we have the minimum version.
set(python_libs)
set(python_version)
set(python_executable)
source_python3_package(python_libs python_version python_executable)

if("${python_version}" VERSION_GREATER_EQUAL "3")
  set(default_enable_python ON)
endif()

# MSan / UBSan fails on Python tests due to linking issue.
if("${CMAKE_BUILD_TYPE}" MATCHES "^(MSan|UBSan)$")
    set(default_enable_python OFF)
endif()

if(NOT HAVE_BUILTIN_EXTENSION_LZ4)
    set(default_enable_lz4 ${HAVE_LIBLZ4})
endif()
if(NOT HAVE_BUILTIN_EXTENSION_SNAPPY)
    set(default_enable_snappy ${HAVE_LIBSNAPPY})
endif()
if(NOT HAVE_BUILTIN_EXTENSION_ZLIB)
    set(default_enable_zlib ${HAVE_LIBZ})
endif()
if(NOT HAVE_BUILTIN_EXTENSION_ZSTD)
    set(default_enable_zstd ${HAVE_LIBZSTD})
endif()

if("${CMAKE_BUILD_TYPE}" STREQUAL "Release")
    set(default_enable_debug_info OFF)
endif()

if(WT_WIN)
    # We force a static compilation to generate a ".lib" file. We can then
    # additionally generate a dll file using a *DEF file.
    set(default_enable_static ON)
    set(default_enable_shared OFF)
endif()

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
        "loongarch64;WT_LOONGARCH64;"
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
    ENABLE_ANTITHESIS
    "Enable the Antithesis random library"
    DEFAULT OFF
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
    "Enable WiredTiger diagnostics. Automatically enables debug info."
    DEFAULT ${default_have_diagnostics}
)

config_bool(
    HAVE_REF_TRACK
    "Enable WiredTiger to track recent state transitions for WT_REF structures (always enabled in diagnostic build)"
    DEFAULT OFF
)

config_bool(
    HAVE_CALL_LOG
    "Enable call log generation"
    DEFAULT OFF
    DEPENDS "HAVE_DIAGNOSTIC"
    DEPENDS_ERROR ON "Call log requires diagnostic build to be enabled"
)

config_bool(
    NON_BARRIER_DIAGNOSTIC_YIELDS
    "Don't set a full barrier when yielding threads in diagnostic mode. Requires diagnostic mode to be enabled."
    DEFAULT OFF
)

config_bool(
    HAVE_UNITTEST
    "Enable C++ Catch2 based WiredTiger unit tests"
    DEFAULT OFF
)

config_bool(
    HAVE_UNITTEST_ASSERTS
    "Enable C++ Catch2 based WiredTiger unit tests. Special configuration for testing assertions"
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
    DEFAULT ${default_enable_static}
)

config_bool(
    ENABLE_SHARED
    "Compile as a shared library"
    DEFAULT ${default_enable_shared}
)

config_bool(
    WITH_PIC
    "Generate position-independent code. Note PIC will always \
    be used on shared targets, irrespective of the value of this configuration."
    DEFAULT ON
)

config_bool(
    ENABLE_STRICT
    "Compile with strict compiler warnings enabled"
    DEFAULT ON
)

config_bool(
    ENABLE_PYTHON
    "Configure the python API"
    DEFAULT ${default_enable_python}
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
    DEFAULT ${default_enable_lz4}
    DEPENDS "HAVE_LIBLZ4"
    # Specifically throw a fatal error if a user tries to enable the lz4 compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find lz4 library"
)

config_bool(
    ENABLE_MEMKIND
    "Enable the memkind library, needed for NVRAM or SSD block caches"
    DEFAULT OFF
    DEPENDS "HAVE_LIBMEMKIND"
    # Specifically throw a fatal error if a user tries to enable the memkind allocator without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find memkind library"
)

config_bool(
    ENABLE_SNAPPY
    "Build the snappy compressor extension"
    DEFAULT ${default_enable_snappy}
    DEPENDS "HAVE_LIBSNAPPY"
    # Specifically throw a fatal error if a user tries to enable the snappy compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find snappy library"
)

config_bool(
    ENABLE_ZLIB
    "Build the zlib compressor extension"
    DEFAULT ${default_enable_zlib}
    DEPENDS "HAVE_LIBZ"
    # Specifically throw a fatal error if a user tries to enable the zlib compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find zlib library"
)

config_bool(
    ENABLE_ZSTD
    "Build the libzstd compressor extension"
    DEFAULT ${default_enable_zstd}
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
    DEFAULT ${default_enable_tcmalloc}
    DEPENDS "HAVE_LIBTCMALLOC"
    # Specifically throw a fatal error if a user tries to enable the tcmalloc allocator without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find tcmalloc library"
)

config_bool(
    ENABLE_CPPSUITE
    "Build the cppsuite"
    DEFAULT ON
)

config_bool(
    ENABLE_LAZYFS
    "Build LazyFS for testing"
    DEFAULT OFF
)

config_bool(
    ENABLE_S3
    "Build the S3 storage extension"
    DEFAULT OFF
)

config_bool(
    ENABLE_GCP
    "Build the Google Cloud Platform storage extension"
    DEFAULT OFF
)

config_bool(
    ENABLE_AZURE
    "Build the Azure storage extension"
    DEFAULT OFF
)

config_bool(
    ENABLE_LLVM
    "Enable compilation of LLVM-based tools and executables i.e. xray & fuzzer."
    DEFAULT OFF
)

config_bool(
    ENABLE_DEBUG_INFO
    "Enable debug information. Will be automatically enabled if diagnostics is enabled."
    DEFAULT ${default_enable_debug_info}
)

set(default_optimize_level "-Og")
if("${CMAKE_BUILD_TYPE}" MATCHES "^(Release|RelWithDebInfo)$")
    if(WT_WIN)
        set(default_optimize_level "/O2")
    else()
        set(default_optimize_level "-O2")
    endif()
else()
    if(WT_WIN)
        set(default_optimize_level "/Od")
    endif()
endif()

config_string(
    CC_OPTIMIZE_LEVEL
    "CC optimization level"
    DEFAULT "${default_optimize_level}"
)

add_compile_options("${CC_OPTIMIZE_LEVEL}")

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

# Diagnostic mode requires debug info, set it automatically.
if (HAVE_DIAGNOSTIC)
    set(ENABLE_DEBUG_INFO ON)
endif()

# Setup debug info if enabled.
if(ENABLE_DEBUG_INFO)
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # Produce full symbolic debugging information.
        add_compile_options(/Z7)
        # Ensure a PDB file can be generated for debugging symbols.
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG")
    else()
        # Higher debug levels `-g3`/`-ggdb3` emit additional debug information, including 
        # macro definitions that allow us to evaluate macros such as `p S2C(session)` inside of gdb.
        # This needs to be in DWARF version 2 format or later - and should be by default - but 
        # we'll specify version 4 here to be safe.
        add_compile_options(-g3)
        add_compile_options(-ggdb3)
        add_compile_options(-gdwarf-4)
        if("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
            # Clang requires one additional flag to output macro debug information.
            add_compile_options(-fdebug-macro)
        endif()
    endif()
endif()

# Ref tracking is always enabled in diagnostic build.
if (HAVE_DIAGNOSTIC AND NOT HAVE_REF_TRACK)
    set(HAVE_REF_TRACK ON CACHE STRING "" FORCE)
    set(HAVE_REF_TRACK_DISABLED OFF CACHE INTERNAL "" FORCE)
endif()

if (NON_BARRIER_DIAGNOSTIC_YIELDS AND NOT HAVE_DIAGNOSTIC)
    message(FATAL_ERROR "`NON_BARRIER_DIAGNOSTIC_YIELDS` can only be enabled when `HAVE_DIAGNOSTIC` is enabled.")
endif()

if (HAVE_UNITTEST_ASSERTS AND NOT HAVE_UNITTEST)
    message(FATAL_ERROR "`HAVE_UNITTEST_ASSERTS` can only be enabled when `HAVE_UNITTEST` is enabled.")
endif()

if(WT_WIN)
    # Check if we a using the dynamic or static run-time library.
    if(DYNAMIC_CRT)
        # Use the multithread-specific and DLL-specific version of the run-time library (MSVCRT.lib).
        add_compile_options(/MD)
    else()
        # Use the multithread, static version of the run-time library.
        add_compile_options(/MT)
    endif()
endif()

if(ENABLE_ANTITHESIS)
    add_compile_options(-fsanitize-coverage=trace-pc-guard)
endif()
