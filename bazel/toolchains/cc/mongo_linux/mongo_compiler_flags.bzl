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
visibility("//bazel")

LINUX_OPT_COPTS = select({
    # This is opt=debug, not to be confused with (opt=on && dbg=on)
    "//bazel/config:gcc_or_clang_opt_debug": [
        "-Og",
    ],
    "//bazel/config:gcc_or_clang_opt_off": [
        "-O0",
    ],
    "//bazel/config:gcc_or_clang_opt_on": [
        "-O2",
    ],
    "//bazel/config:gcc_or_clang_opt_size": [
        "-Os",
    ],
    "//conditions:default": [],
})

GCC_OR_CLANG_WARNINGS_COPTS = select({
    "//bazel/config:gcc_or_clang": [
        # Enable all warnings by default.
        "-Wall",

        # Warn on comparison between signed and unsigned integer expressions.
        "-Wsign-compare",

        # Do not warn on unknown pragmas.
        "-Wno-unknown-pragmas",

        # Warn if a precompiled header (see Precompiled Headers) is found in the
        # search path but can't be used.
        "-Winvalid-pch",

        # This warning was added in g++-4.8.
        "-Wno-unused-local-typedefs",

        # Clang likes to warn about unused functions, which seems a tad
        # aggressive and breaks -Werror, which we want to be able to use.
        "-Wno-unused-function",

        # Prevents warning about using deprecated features (such as auto_ptr in
        # c++11) Using -Wno-error=deprecated-declarations does not seem to work
        # on some compilers, including at least g++-4.6.
        "-Wno-deprecated-declarations",

        # New in clang-3.4, trips up things mostly in third_party, but in a few
        # places in the primary mongo sources as well.
        "-Wno-unused-const-variable",

        # This has been suppressed in gcc 4.8, due to false positives, but not
        # in clang. So we explicitly disable it here.
        "-Wno-missing-braces",

        # SERVER-76472 we don't try to maintain ABI so disable warnings about
        # possible ABI issues.
        "-Wno-psabi",
    ],
    "//conditions:default": [],
})

CLANG_WARNINGS_COPTS = select({
    "//bazel/config:compiler_type_clang": [
        # SERVER-44856: Our windows builds complain about unused
        # exception parameters, but GCC and clang don't seem to do
        # that for us automatically. In the interest of making it more
        # likely to catch these errors early, add the (currently clang
        # only) flag that turns it on.
        "-Wunused-exception-parameter",

        # Clang likes to warn about unused private fields, but some of our
        # third_party libraries have such things.
        "-Wno-unused-private-field",

        # As of clang-3.4, this warning appears in v8, and gets escalated to an
        # error.
        "-Wno-tautological-constant-out-of-range-compare",

        # As of clang in Android NDK 17, these warnings appears in boost and/or
        # ICU, and get escalated to errors
        "-Wno-tautological-constant-compare",
        "-Wno-tautological-unsigned-zero-compare",
        "-Wno-tautological-unsigned-enum-zero-compare",

        # Suppress warnings about not consistently using override everywhere in
        # a class. It seems very pedantic, and we have a fair number of
        # instances.
        "-Wno-inconsistent-missing-override",

        # Don't issue warnings about potentially evaluated expressions
        "-Wno-potentially-evaluated-expression",

        # Disable warning about templates that can't be implicitly instantiated.
        # It is an attempt to make a link error into an easier-to-debug compiler
        # failure, but it triggers false positives if explicit instantiation is
        # used in a TU that can see the full definition. This is a problem at
        # least for the S2 headers.
        "-Wno-undefined-var-template",

        # This warning was added in clang-4.0, but it warns about code that is
        # required on some platforms. Since the warning just states that
        # 'explicit instantiation of [a template] that occurs after an explicit
        # specialization has no effect', it is harmless on platforms where it
        # isn't required
        "-Wno-instantiation-after-specialization",

        # This warning was added in clang-5 and flags many of our lambdas. Since
        # it isn't actively harmful to capture unused variables we are
        # suppressing for now with a plan to fix later.
        "-Wno-unused-lambda-capture",

        # This warning was added in Apple clang version 11 and flags many
        # explicitly defaulted move constructors and assignment operators for
        # being implicitly deleted, which is not useful.
        "-Wno-defaulted-function-deleted",
    ],
    "//conditions:default": [],
})

GCC_WARNINGS_COPTS = select({
    "//bazel/config:compiler_type_gcc": [
        # Disable warning about variables that may not be initialized
        # Failures are triggered in the case of boost::optional
        "-Wno-maybe-uninitialized",

        # Prevents warning about unused but set variables found in boost version
        # 1.49 in boost/date_time/format_date_parser.hpp which does not work for
        # compilers GCC >= 4.6. Error explained in
        # https://svn.boost.org/trac/boost/ticket/6136 .
        "-Wno-unused-but-set-variable",
    ],
    "//conditions:default": [],
})

CLANG_FNO_LIMIT_DEBUG_INFO = select({
    "//bazel/config:compiler_type_clang": [
        # We add this flag to make clang emit debug info for c++ stl types so
        # that our pretty printers will work with newer clang's which omit this
        # debug info. This does increase the overall debug info size.
        "-fno-limit-debug-info",
    ],
    "//conditions:default": [],
})

GCC_OR_CLANG_GENERAL_COPTS = select({
    "//bazel/config:gcc_or_clang": [
        # Generate unwind table in DWARF format, if supported by target machine.
        # The table is exact at each instruction boundary, so it can be used for
        # stack unwinding from asynchronous events (such as debugger or garbage
        # collector).
        "-fasynchronous-unwind-tables",

        # For debug builds with tcmalloc, we need the frame pointer so it can
        # record the stack of allocations. We also need the stack pointer for
        # stack traces unless libunwind is enabled. Enable frame pointers by
        # default.
        "-fno-omit-frame-pointer",

        # Enable strong by default, this may need to be softened to
        # -fstack-protector-all if we run into compatibility issues.
        "-fstack-protector-strong",

        # Disable TBAA optimization
        "-fno-strict-aliasing",

        # Show colors even though bazel captures stdout/stderr
        "-fdiagnostics-color",
    ],
    "//conditions:default": [],
})

LINUX_PTHREAD_LINKFLAG = select({
    "@platforms//os:linux": [
        # Adds support for multithreading with the pthreads library. This option
        # sets flags for both the preprocessor and linker.
        "-pthread",
    ],
    "//conditions:default": [],
})

RDYNAMIC_LINKFLAG = select({
    # Use rdynamic for backtraces with glibc unless we have libunwind.
    "@platforms//os:linux": [
        # Pass the flag -export-dynamic to the ELF linker, on targets that
        # support it. This instructs the linker to add all symbols, not only
        # used ones, to the dynamic symbol table. This option is needed for some
        # uses of dlopen or to allow obtaining backtraces from within a program.
        "-rdynamic",
    ],
    "//conditions:default": [],
})

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

# Disable floating-point contractions such as forming of fused multiply-add
# operations.
FLOATING_POINT_COPTS = select({
    "//bazel/config:compiler_type_clang": ["-ffp-contract=off"],
    "//bazel/config:compiler_type_gcc": ["-ffp-contract=off"],

    # msvc defaults to /fp:precise. Visual Studio 2022 does not emit
    # floating-point contractions with /fp:precise, but previous versions can.
    # Disable contractions altogether by using /fp:strict.
    "//bazel/config:compiler_type_msvc": ["/fp:strict"],
})

IMPLICIT_FALLTHROUGH_COPTS = select({
    "//bazel/config:compiler_type_clang": ["-Wimplicit-fallthrough"],
    "//bazel/config:compiler_type_gcc": ["-Wimplicit-fallthrough=5"],
    "//conditions:default": [],
})

LIBCXX_COPTS = select({
    "//bazel/config:use_libcxx_required_settings": ["-stdlib=libc++"],
    "//bazel/config:use_libcxx_disabled": [],
}, no_match_error = LIBCXX_ERROR_MESSAGE)

LIBCXX_LINKFLAGS = LIBCXX_COPTS

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

LINUX_EXTRA_GLOBAL_LIBS_LINKFLAGS = select({
    "@platforms//os:linux": [
        "-lm",
        "-lresolv",
        "-latomic",
    ],
    "//conditions:default": [],
})

DEBUG_TYPES_SECTION_FLAGS = select({
    "//bazel/config:linux_clang_linkstatic": [
        "-fdebug-types-section",
    ],
    # SUSE15 gcc builds system libraries with dwarf32 and needs -fdebug-types-section to keep
    # the size of the debug information under the 4GB limit.
    "//bazel/config:suse15_gcc_linkstatic": [
        "-fdebug-types-section",
    ],
    "//conditions:default": [],
})

GCC_OR_CLANG_LINKFLAGS = select({
    "//bazel/config:linux_gcc_or_clang": [
        # Explicitly enable GNU build id's if the linker supports it.
        "-Wl,--build-id",

        # Explicitly use the new gnu hash section if the linker offers it.
        "-Wl,--hash-style=gnu",

        # Disallow an executable stack. Also, issue a warning if any files are
        # found that would cause the stack to become executable if the
        # noexecstack flag was not in play, so that we can find them and fix
        # them. We do this here after we check for ld.gold because the
        # --warn-execstack is currently only offered with gold.
        "-Wl,-z,noexecstack",
        "-Wl,--warn-execstack",

        # If possible with the current linker, mark relocations as read-only.
        "-Wl,-z,relro",
    ],
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

COMPRESS_DEBUG_LINKFLAGS = select({
    # Disable debug compression in both the assembler and linker
    # by default.
    "@platforms//os:linux": [
        "-Wl,--compress-debug-sections=none",
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
    "//conditions:default": [],
})

SYMBOL_ORDER_LINKFLAGS = select({
    "//bazel/config:symbol_ordering_file_enabled": [
        "-Wl,--symbol-ordering-file=$(location //:symbols.orderfile)",
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

SHARED_ARCHIVE_LINKFLAGS = select({
    "//bazel/config:shared_archive_enabled_gcc": [
        "-Wl,-Bsymbolic",
        "-Wl,--no-gnu-unique",
    ],
    "//conditions:default": [],
})

# Passed to both the compiler and linker
COVERAGE_FLAGS = select({
    "//bazel/config:gcov_enabled": ["--coverage", "-fprofile-update=single"],
    "//conditions:default": [],
})

# Passed to both the compiler and linker
PGO_PROFILE_FLAGS = select({
    "//bazel/config:pgo_profile_enabled": [
        "-fprofile-instr-generate",
    ],
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
    LIBCXX_COPTS +
    ADDRESS_SANITIZER_COPTS +
    MEMORY_SANITIZER_COPTS +
    FUZZER_SANITIZER_COPTS +
    UNDEFINED_SANITIZER_COPTS +
    THREAD_SANITIZER_COPTS +
    ANY_SANITIZER_AVAILABLE_COPTS +
    LINUX_OPT_COPTS +
    GCC_OR_CLANG_WARNINGS_COPTS +
    GCC_OR_CLANG_GENERAL_COPTS +
    FLOATING_POINT_COPTS +
    CLANG_WARNINGS_COPTS +
    CLANG_FNO_LIMIT_DEBUG_INFO +
    COMPRESS_DEBUG_COPTS +
    DEBUG_TYPES_SECTION_FLAGS +
    IMPLICIT_FALLTHROUGH_COPTS +
    MTUNE_MARCH_COPTS +
    DISABLE_SOURCE_WARNING_AS_ERRORS_COPTS +
    THIN_LTO_FLAGS +
    SYMBOL_ORDER_COPTS +
    GCC_WARNINGS_COPTS +
    COVERAGE_FLAGS +
    PGO_PROFILE_FLAGS +
    SHARED_ARCHIVE_COPTS +
    RUNNING_THROUGH_BAZELISK_CHECK
)

MONGO_LINUX_CC_LINKFLAGS = (
    MEMORY_SANITIZER_LINKFLAGS +
    ADDRESS_SANITIZER_LINKFLAGS +
    FUZZER_SANITIZER_LINKFLAGS +
    UNDEFINED_SANITIZER_LINKFLAGS +
    THREAD_SANITIZER_LINKFLAGS +
    LIBCXX_LINKFLAGS +
    DETECT_ODR_VIOLATIONS_LINKFLAGS +
    BIND_AT_LOAD_LINKFLAGS +
    RDYNAMIC_LINKFLAG +
    LINUX_PTHREAD_LINKFLAG +
    ANY_SANITIZER_AVAILABLE_LINKFLAGS +
    ANY_SANITIZER_GCC_LINKFLAGS +
    GCC_OR_CLANG_LINKFLAGS +
    COMPRESS_DEBUG_LINKFLAGS +
    DEBUG_TYPES_SECTION_FLAGS +
    DISABLE_SOURCE_WARNING_AS_ERRORS_LINKFLAGS +
    THIN_LTO_FLAGS +
    SYMBOL_ORDER_LINKFLAGS +
    COVERAGE_FLAGS +
    PGO_PROFILE_FLAGS +
    SANITIZE_WITHOUT_TSAN_LINKFLAGS +
    SHARED_ARCHIVE_LINKFLAGS +
    LIBGCC_LINKFLAGS +
    LINUX_EXTRA_GLOBAL_LIBS_LINKFLAGS
)
