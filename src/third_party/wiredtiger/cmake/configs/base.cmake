include(cmake/helpers.cmake)
include(cmake/configs/version.cmake)

# Setup defaults based on the build type and available libraries.
set(default_have_diagnostics ON)
set(default_enable_python OFF)
set(default_enable_lz4 OFF)
set(default_enable_snappy OFF)
set(default_enable_zlib OFF)
set(default_enable_zstd OFF)
set(default_enable_iaa OFF)
set(default_enable_debug_info ON)
set(default_enable_static OFF)
set(default_enable_shared ON)
set(default_internal_sqlite3 ON)

string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)

if(${CMAKE_BUILD_TYPE_UPPER} MATCHES "^(RELEASE|RELWITHDEBINFO)$")
    set(default_have_diagnostics OFF)
endif()

# Enable python if we have the minimum version.
find_package(Python3 QUIET COMPONENTS Interpreter Development)

if(Python3_FOUND)
  set(default_enable_python ON)
endif()

# MSan / UBSan fails on Python tests due to linking issue.
if(${CMAKE_BUILD_TYPE_UPPER} MATCHES "^(MSAN|UBSAN)$")
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
if(NOT HAVE_BUILTIN_EXTENSION_IAA)
    set(default_enable_iaa ${HAVE_LIBQPL})
endif()

if(${CMAKE_BUILD_TYPE_UPPER} STREQUAL "RELEASE")
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
    DEPENDS "WT_POSIX"
)

config_bool(
    WT_POSIX
    "Is a posix platform"
    DEFAULT ON
    DEPENDS "WT_LINUX OR WT_DARWIN"
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
    CODE_COVERAGE_MEASUREMENT
    "Enable alternative code that is specifically used when measuring code coverage"
    DEFAULT OFF
)

config_bool(
    INLINE_FUNCTIONS_INSTEAD_OF_MACROS
    "Switch from macros to inline functions where available"
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
    ENABLE_COLORIZE_OUTPUT
    "Compile with build error colors enabled"
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
    DEPENDS "ENABLE_PYTHON"
)

config_string(
    SWIG_REQUIRED_VERSION
    "SWIG version to use when building Python bindings. \
    Expected format of version string: major[.minor[.patch]]"
    DEFAULT "4"
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
    ENABLE_IAA
    "Build the libqpl compressor extension"
    DEFAULT ${default_enable_iaa}
    DEPENDS "HAVE_LIBQPL"
    # Specifically throw a fatal error if a user tries to enable the qpl compressor without
    # actually having the library available (as opposed to silently defaulting to OFF).
    DEPENDS_ERROR ON "Failed to find qpl library"
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
    ENABLE_MODEL
    "Build the model for lightweight formal verification"
    DEFAULT ON
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

config_string(
    SQLITE3_REQUIRED_VERSION
    "SQLite3 version to use when building PALite extension. \
    Expected format of version string: major[.minor[.patch]]"
    DEFAULT "3.8"   # Minimum version for partial indexes (used in PALite)
)

# Use system provided sqlite3 if available.
find_package(SQLite3 ${SQLITE3_REQUIRED_VERSION} QUIET)

if(SQLite3_FOUND)
    set(default_internal_sqlite3 OFF)
endif()

config_bool(
    ENABLE_INTERNAL_SQLITE3
    "Enable internal SQLite3 library. If disabled, the system SQLite3 library will be used."
    DEFAULT ${default_internal_sqlite3}
)

set(default_optimize_level "-Og")
if(${CMAKE_BUILD_TYPE_UPPER} MATCHES "^(RELEASE|RELWITHDEBINFO)$")
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
# Only set initial debug flags once to preserve user customizations.
if(ENABLE_DEBUG_INFO AND NOT WT_DEBUG_FLAGS_INITIALIZED)
    set(BUILD_TYPES_WITH_DEBUG_INFO ${BUILD_MODES})
    list(REMOVE_ITEM BUILD_TYPES_WITH_DEBUG_INFO Release)

    set(DEBUG_INFO_FLAGS)
    if(GNU_C_COMPILER OR CLANG_C_COMPILER)
        # Higher debug levels `-g3`/`-ggdb3` emit additional debug information, including
        # macro definitions that allow us to evaluate macros such as `p S2C(session)` inside of gdb.
        # This needs to be in DWARF version 2 format or later - and should be by default - but
        # we'll specify version 4 here to be safe.
        list(APPEND DEBUG_INFO_FLAGS -g3 -gdwarf-4)
        if(CLANG_C_COMPILER)
            # Clang requires one additional flag to output macro debug information.
            list(APPEND DEBUG_INFO_FLAGS -glldb -fdebug-macro)
        else()
            list(APPEND DEBUG_INFO_FLAGS -ggdb3)
        endif()

        add_cmake_compiler_flags(
            FLAGS ${DEBUG_INFO_FLAGS}
            LANGUAGES C CXX
            BUILD_TYPES ${BUILD_TYPES_WITH_DEBUG_INFO}
        )
    endif()

    # MSVC: ensure linker produces PDBs.
    if(MSVC_C_COMPILER)
        add_cmake_linker_flags(
            FLAGS "/DEBUG"
            BINARIES EXE SHARED
            BUILD_TYPES ${BUILD_TYPES_WITH_DEBUG_INFO}
        )
    endif()

    # Mark that we've set the initial debug flags
    set(WT_DEBUG_FLAGS_INITIALIZED TRUE CACHE INTERNAL
        "WiredTiger debug flags have been initialized")
endif()

# We want to use the optimization level from CC_OPTIMIZE_LEVEL.
if(NOT ("${WT_OPTIMIZE_FLAGS_SAVED}" STREQUAL "${CC_OPTIMIZE_LEVEL}"))
    if(MSVC_C_COMPILER)
        set(opt_flags "/O3" "/O2")
    else()
        set(opt_flags "-O3" "-O2")
    endif()
    set(prev_opt_flags "${WT_OPTIMIZE_FLAGS_SAVED}")
    separate_arguments(prev_opt_flags)
    list(APPEND opt_flags ${prev_opt_flags})

    set(new_opt_flags "${CC_OPTIMIZE_LEVEL}")
    separate_arguments(new_opt_flags)

    foreach(lang C CXX)
        foreach(build_type RELEASE RELWITHDEBINFO)
            replace_compile_options(CMAKE_${lang}_FLAGS_${build_type}
                REMOVE ${opt_flags}
                ADD ${new_opt_flags})
        endforeach()
    endforeach()

    if(GNU_C_COMPILER OR GNU_CXX_COMPILER)
        foreach(lang C CXX)
            add_cmake_flag(CMAKE_${lang}_FLAGS -fno-strict-aliasing)
        endforeach()
    endif()

    # Mark that we've set the initial optimize flags
    set(WT_OPTIMIZE_FLAGS_SAVED "${CC_OPTIMIZE_LEVEL}" CACHE INTERNAL
        "WiredTiger optimize flags have been initialized")
endif()

# Ref tracking is always enabled in diagnostic build.
if (HAVE_DIAGNOSTIC AND NOT HAVE_REF_TRACK)
    set(HAVE_REF_TRACK ON CACHE BOOL "" FORCE)
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
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    else()
        # Use the multithread, static version of the run-time library.
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
endif()

if(ENABLE_ANTITHESIS)
    foreach(lang C CXX)
        add_cmake_flag(CMAKE_${lang}_FLAGS -fsanitize-coverage=trace-pc-guard)
    endforeach()
endif()
