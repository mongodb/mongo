"""This file contains compiler flags that are specific to linux cc compiling and linking."""

load(
    "//bazel/toolchains/cc:mongo_errors.bzl",
    "BAZELISK_CHECK_ERROR_MESSAGE",
    "DETECT_ODR_VIOLATIONS_ERROR_MESSAGE",
    "LIBCXX_ERROR_MESSAGE",
    "REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE",
    "SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE",
    "THREAD_SANITIZER_ERROR_MESSAGE",
)

# Flags listed in this file is only visible to the bazel build system.
visibility("//bazel/toolchains/cc")

# SERVER-9761: Ensure early detection of missing symbols in dependent libraries
# at program startup. For non-release dynamic builds we disable this behavior in
# the interest of improved mongod startup times. Xcode15 removed bind_at_load
# functionality so we cannot have a selection for macosx here
# ld: warning: -bind_at_load is deprecated on macOS
# TODO: SERVER-90596 reenable loading at startup
BIND_AT_LOAD_LINKFLAGS = select({
    "//bazel/config:linkstatic_enabled_linux": [
        "-Wl,-z,now",
    ],
    "//conditions:default": [],
})

IMPLICIT_FALLTHROUGH_COPTS = select({
    "//bazel/config:compiler_type_clang": ["-Wimplicit-fallthrough"],
    "//bazel/config:compiler_type_gcc": ["-Wimplicit-fallthrough=5"],
    "//conditions:default": [],
})

# -fno-omit-frame-pointer should be added if any sanitizer flag is used by user
ANY_SANITIZER_AVAILABLE_COPTS = select({
    "//bazel/config:any_sanitizer_required_setting": [
        "-fno-omit-frame-pointer",
    ],
    "//bazel/config:no_enabled_sanitizer": [],
}, no_match_error = REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE)

ANY_SANITIZER_AVAILABLE_LINKFLAGS = select({
    # Sanitizer libs may inject undefined refs (for hooks) at link time, but the
    # symbols will be available at runtime via the compiler runtime lib.
    "//bazel/config:any_sanitizer_required_setting": [
        "-Wl,--allow-shlib-undefined",
    ],
    "//bazel/config:no_enabled_sanitizer": [],
}, no_match_error = REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE)

ANY_SANITIZER_GCC_LINKFLAGS = select({
    # GCC's implementation of ASAN depends on libdl.
    "//bazel/config:any_sanitizer_gcc": ["-ldl"],
    "//conditions:default": [],
})

ADDRESS_SANITIZER_COPTS = select({
    "//bazel/config:asan_disabled": [],
    "//bazel/config:sanitize_address_required_settings": [
        "-fsanitize=address",
        "-fsanitize-blacklist=$(location //etc:asan_denylist_h)",
    ],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE)

ADDRESS_SANITIZER_LINKFLAGS = select({
    "//bazel/config:asan_disabled": [],
    "//bazel/config:sanitize_address_required_settings": ["-fsanitize=address"],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE)

# Makes it easier to debug memory failures at the cost of some perf:
#   -fsanitize-memory-track-origins
MEMORY_SANITIZER_COPTS = select({
    "//bazel/config:msan_disabled": [],
    "//bazel/config:sanitize_memory_required_settings": [
        "-fsanitize=memory",
        "-fsanitize-memory-track-origins",
        "-fsanitize-blacklist=$(location //etc:msan_denylist_h)",
    ],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE)

SANITIZE_WITHOUT_TSAN_LINKFLAGS = select({
    "//bazel/config:sanitize_without_tsan": [
        "-rtlib=compiler-rt",
        "-unwindlib=libgcc",
    ],
    "//conditions:default": [],
})

# Makes it easier to debug memory failures at the cost of some perf:
#   -fsanitize-memory-track-origins
MEMORY_SANITIZER_LINKFLAGS = select({
    "//bazel/config:sanitize_memory_required_settings": ["-fsanitize=memory"],
    "//bazel/config:msan_disabled": [],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE)

# We can't include the fuzzer flag with the other sanitize flags. The libfuzzer
# library already has a main function, which will cause the dependencies check
# to fail
FUZZER_SANITIZER_COPTS = select({
    "//bazel/config:sanitize_fuzzer_required_settings": [
        "-fsanitize=fuzzer-no-link",
        "-fprofile-instr-generate",
        "-fcoverage-mapping",
    ],
    "//bazel/config:fsan_disabled": [],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE + "fuzzer")

# These flags are needed to generate a coverage report
FUZZER_SANITIZER_LINKFLAGS = select({
    "//bazel/config:sanitize_fuzzer_required_settings": [
        "-fsanitize=fuzzer-no-link",
        "-fprofile-instr-generate",
        "-fcoverage-mapping",
        "-nostdlib++",
        "-lstdc++",
    ],
    "//bazel/config:fsan_disabled": [],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE + "fuzzer")

# Combines following two conditions -
# 1.
# TODO: SERVER-48622
#
# See https://github.com/google/sanitizers/issues/943 for why we disallow
# combining TSAN with libunwind. We could, aternatively, have added logic to
# automate the decision about whether to enable libunwind based on whether TSAN
# is enabled, but that logic is already complex, and it feels better to make it
# explicit that using TSAN means you won't get the benefits of libunwind.
#
# 2.
# We add suppressions based on the library file in etc/tsan.suppressions so the
# link-model needs to be dynamic.

THREAD_SANITIZER_COPTS = select({
    "//bazel/config:sanitize_thread_required_settings": [
        "-fsanitize=thread",
        "-fsanitize-blacklist=$(location //etc:tsan_denylist_h)",
    ],
    "//bazel/config:tsan_disabled": [],
}, no_match_error = THREAD_SANITIZER_ERROR_MESSAGE)

THREAD_SANITIZER_LINKFLAGS = select({
    "//bazel/config:sanitize_thread_required_settings": ["-fsanitize=thread"],
    "//bazel/config:tsan_disabled": [],
}, no_match_error = THREAD_SANITIZER_ERROR_MESSAGE)

# By default, undefined behavior sanitizer doesn't stop on the first error. Make
# it so. Newer versions of clang have renamed the flag. However, this flag
# cannot be included when using the fuzzer sanitizer if we want to suppress
# errors to uncover new ones.

# In dynamic builds, the `vptr` sanitizer check can require additional
# dependency edges. That is very inconvenient, because such builds can't use
# z,defs. The result is a very fragile link graph, where refactoring the link
# graph in one place can have surprising effects in others. Instead, we just
# disable the `vptr` sanitizer for dynamic builds. We tried some other
# approaches in SERVER-49798 of adding a new descriptor type, but that didn't
# address the fundamental issue that the correct link graph for a dynamic+ubsan
# build isn't the same as the correct link graph for a regular dynamic build.

UNDEFINED_SANITIZER_COPTS = select({
    "//bazel/config:ubsan_enabled": ["-fsanitize=undefined"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:sanitize_undefined_dynamic_link_settings": [
        "-fno-sanitize=vptr",
    ],
    "//conditions:default": [],
}) + select({
    "//bazel/config:sanitize_undefined_without_fuzzer_settings": [
        "-fno-sanitize-recover",
    ],
    "//conditions:default": [],
}) + select({
    "//bazel/config:ubsan_enabled": [
        "-fsanitize-blacklist=$(location //etc:ubsan_denylist_h)",
    ],
    "//conditions:default": [],
})

UNDEFINED_SANITIZER_LINKFLAGS = select({
    "//bazel/config:ubsan_enabled": ["-fsanitize=undefined"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:sanitize_undefined_dynamic_link_settings": [
        "-fno-sanitize=vptr",
    ],
    "//conditions:default": [],
})

DETECT_ODR_VIOLATIONS_LINKFLAGS = select({
    "//bazel/config:detect_odr_violations_required_settings": [
        "-Wl,--detect-odr-violations",
    ],
    "//bazel/config:detect_odr_violations_disabled": [],
}, no_match_error = DETECT_ODR_VIOLATIONS_ERROR_MESSAGE)

# TODO(SERVER-101099): Remove this once builds are containerized and system libraries inside the containers
# no longer contain debug symbols.
#
# In RHEL8 and RHEL9 the debug symbols for libgcc aren't stripped and are instead split, which still leaves behind
# debug symbols in the libgcc shared object file. These debug symbols are created with gdwarf32, so they're limited to
# a 32 bit address space. Even if the mongodb binaries are compiled with gdwarf64, there's a chance that the gdwarf32
# libgcc debug symbols will be placed after the gdwarf64 debug symbols. This started happening in the RHEL9 ppc64le
# build.
#
# The workaround for this is stripping the debug symbols from libgcc and statically compiling the libgcc from the
# toolchain into the mongodb binaries. The longer term solution for this is to containerize the non-remote-execution
# build and strip the debug symbols inside the container, or patch the compilers to properly order gdwarf32 symbols
# before gdwarf64 symbols. See https://reviews.llvm.org/D96144
LIBGCC_LINKFLAGS = select({
    "//bazel/config:rhel9_ppc64le_gcc_linkstatic": ["-static-libgcc"],
    "//conditions:default": [],
})

COMPRESS_DEBUG_COPTS = select({
    # Debug compression significantly reduces .o, .dwo, and .a sizes
    "//bazel/config:compress_debug_compile_enabled": [
        "-Wa,--compress-debug-sections",
    ],
    # explicitly disable compression if its not enabled or else not passing the flag
    # by default still compresses on x86/x86_64 - nocompress is only a flag in gcc not clang
    "//bazel/config:compress_debug_compile_disabled_linux_gcc": [
        "-Wa,--nocompress-debug-sections",
    ],
    "//conditions:default": [],
})

DISABLE_SOURCE_WARNING_AS_ERRORS_COPTS = select({
    "//bazel/config:disable_warnings_as_errors_linux": ["-Werror"],
    # TODO(SERVER-90183): Enable once MacOS has a custom Bazel toolchain config.
    # "//bazel/config:disable_warnings_as_errors_macos": ["-Werror"],
    "//bazel/config:disable_warnings_as_errors_windows": ["/WX"],
    "//bazel/config:warnings_as_errors_disabled": [],
    "//conditions:default": [],
})

DISABLE_SOURCE_WARNING_AS_ERRORS_LINKFLAGS = select({
    "//bazel/config:disable_warnings_as_errors_linux": ["-Wl,--fatal-warnings"],
    "//bazel/config:warnings_as_errors_disabled": [],
    "//conditions:default": [],
})

MTUNE_MARCH_COPTS = select({
    "//bazel/config:linux_aarch64": [
        "-march=armv8.2-a",
        "-mtune=generic",
    ],
    "//bazel/config:linux_ppc64le": [
        "-mcpu=power8",
        "-mtune=power8",
        "-mcmodel=medium",
    ],
    "//bazel/config:linux_s390x": [
        "-march=z196",
        "-mtune=zEC12",
    ],
    # If we are enabling vectorization in sandybridge mode, we'd rather not hit
    # the 256 wide vector instructions because the heavy versions can cause
    # clock speed reductions.
    "//bazel/config:linux_x86_64": [
        "-march=sandybridge",
        "-mtune=generic",
        "-mprefer-vector-width=128",
    ],
    "//conditions:default": [],
})

THIN_LTO_FLAGS = select({
    "//bazel/config:thin_lto_enabled": ["-flto=thin"],
    "//conditions:default": [],
})

SYMBOL_ORDER_COPTS = select({
    "//bazel/config:symbol_ordering_file_enabled": ["-ffunction-sections"],
    "//bazel/config:symbol_ordering_file_enabled_al2023": ["-ffunction-sections"],
    "//conditions:default": [],
})

SYMBOL_ORDER_LINKFLAGS = select({
    "//bazel/config:symbol_ordering_file_enabled": [
        "-Wl,--symbol-ordering-file=$(location //buildscripts:symbols.orderfile)",
        "-Wl,--no-warn-symbol-ordering",
    ],
    "//bazel/config:symbol_ordering_file_enabled_al2023": [
        "-Wl,--symbol-ordering-file=$(location //buildscripts:symbols-al2023.orderfile)",
        "-Wl,--no-warn-symbol-ordering",
    ],
    "//conditions:default": [],
})

SHARED_ARCHIVE_COPTS = select({
    "//bazel/config:shared_archive_enabled_gcc": [
        "-fno-gnu-unique",
    ],
    "//conditions:default": [],
})

SHARED_ARCHIVE_LINKFLAGS_GNU_UNIQUE = select({
    "//bazel/config:shared_archive_enabled_gcc_not_mold": [
        "-Wl,--no-gnu-unique",
    ],
    "//conditions:default": [],
})

SHARED_ARCHIVE_LINKFLAGS_B_SYMBOLIC = select({
    "//bazel/config:shared_archive_enabled_gcc": [
        "-Wl,-Bsymbolic",
    ],
    "//conditions:default": [],
})

# Passed to both the compiler and linker
COVERAGE_FLAGS = select({
    "//bazel/config:gcov_enabled": ["--coverage", "-fprofile-update=single"],
    "//conditions:default": [],
})

# Hack to throw an error if the user isn't running bazel through bazelisk,
# since we want to make sure the hook inside of tools/bazel gets run.
RUNNING_THROUGH_BAZELISK_CHECK = select({
    "//bazel/config:running_through_bazelisk_x86_64_or_arm64": [],
    "@platforms//cpu:s390x": [],
    "@platforms//cpu:ppc": [],
}, no_match_error = BAZELISK_CHECK_ERROR_MESSAGE)

MONGO_GLOBAL_INCLUDE_DIRECTORIES = [
    "-Isrc",
    "-I$(GENDIR)/src",
    "-I$(GENDIR)/src/mongo/db/modules/enterprise/src",
    "-Isrc/mongo/db/modules/enterprise/src",
    "-Isrc/third_party/valgrind/include",
]

MONGO_LINUX_CC_COPTS = (
    MONGO_GLOBAL_INCLUDE_DIRECTORIES +
    ADDRESS_SANITIZER_COPTS +
    MEMORY_SANITIZER_COPTS +
    FUZZER_SANITIZER_COPTS +
    UNDEFINED_SANITIZER_COPTS +
    THREAD_SANITIZER_COPTS +
    ANY_SANITIZER_AVAILABLE_COPTS +
    COMPRESS_DEBUG_COPTS +
    IMPLICIT_FALLTHROUGH_COPTS +
    MTUNE_MARCH_COPTS +
    DISABLE_SOURCE_WARNING_AS_ERRORS_COPTS +
    THIN_LTO_FLAGS +
    SYMBOL_ORDER_COPTS +
    COVERAGE_FLAGS +
    SHARED_ARCHIVE_COPTS +
    RUNNING_THROUGH_BAZELISK_CHECK
)

MONGO_LINUX_CC_LINKFLAGS = (
    MEMORY_SANITIZER_LINKFLAGS +
    ADDRESS_SANITIZER_LINKFLAGS +
    FUZZER_SANITIZER_LINKFLAGS +
    UNDEFINED_SANITIZER_LINKFLAGS +
    THREAD_SANITIZER_LINKFLAGS +
    DETECT_ODR_VIOLATIONS_LINKFLAGS +
    BIND_AT_LOAD_LINKFLAGS +
    ANY_SANITIZER_AVAILABLE_LINKFLAGS +
    ANY_SANITIZER_GCC_LINKFLAGS +
    DISABLE_SOURCE_WARNING_AS_ERRORS_LINKFLAGS +
    THIN_LTO_FLAGS +
    SYMBOL_ORDER_LINKFLAGS +
    COVERAGE_FLAGS +
    SANITIZE_WITHOUT_TSAN_LINKFLAGS +
    SHARED_ARCHIVE_LINKFLAGS_GNU_UNIQUE +
    SHARED_ARCHIVE_LINKFLAGS_B_SYMBOLIC +
    LIBGCC_LINKFLAGS
)
