"""This module provides a list of defines that is passed in to compiling.
"""

load(
    "//bazel/toolchains/cc:mongo_errors.bzl",
    "GLIBCXX_DEBUG_ERROR_MESSAGE",
    "LIBCXX_ERROR_MESSAGE",
    "SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE",
    "THREAD_SANITIZER_ERROR_MESSAGE",
)

# Defines are only visible to within //bazel directory where
# toolchains and rules are defined.
# TODO: define mongo_generate_config_header rule to hide
# all the compiler options.
visibility([
    "//src/mongo/util",
    "//bazel",
])

WINDOWS_DEFINES = select({
    "@platforms//os:windows": [
        # This tells the Windows compiler not to link against the .lib files and
        # to use boost as a bunch of header-only libraries.
        "BOOST_ALL_NO_LIB",
        "_UNICODE",
        "UNICODE",

        # Temporary fixes to allow compilation with VS2017.
        "_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING",
        "_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",

        # TODO(SERVER-60151): Until we are fully in C++20 mode, it is easier to
        # simply suppress C++20 deprecations. After we have switched over we
        # should address any actual deprecated usages and then remove this flag.
        "_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS",
        "_CONSOLE",
        "_CRT_SECURE_NO_WARNINGS",
        "_ENABLE_EXTENDED_ALIGNED_STORAGE",
        "_SCL_SECURE_NO_WARNINGS",
    ],
    "//conditions:default": [],
})

LINUX_DEFINES = select({
    "@platforms//os:linux": [
        # On linux, C code compiled with gcc/clang -std=c11 causes
        # __STRICT_ANSI__ to be set, and that drops out all of the feature test
        # definitions, resulting in confusing errors when we run C language
        # configure checks and expect to be able to find newer POSIX things.
        # Explicitly enabling _XOPEN_SOURCE fixes that, and should be mostly
        # harmless as on Linux, these macros are cumulative. The C++ compiler
        # already sets _XOPEN_SOURCE, and, notably, setting it again does not
        # disable any other feature test macros, so this is safe to do. Other
        # platforms like macOS and BSD have crazy rules, so don't try this
        # there.
        #
        # Furthermore, as both C++ compilers appear to define _GNU_SOURCE
        # unconditionally (because libstdc++ requires it), it seems prudent to
        # explicitly add that too, so that C language checks see a consistent
        # set of definitions.
        "_XOPEN_SOURCE=700",
        "_GNU_SOURCE",
    ],
    "//conditions:default": [],
})

MACOS_DEFINES = select({
    "@platforms//os:macos": [
        # TODO SERVER-54659 - ASIO depends on std::result_of which was removed
        # in C++ 20. xcode15 does not have backwards compatibility.
        "ASIO_HAS_STD_INVOKE_RESULT",
        # This is needed to compile boost on the newer xcodes
        "BOOST_NO_CXX98_FUNCTION_BASE",
    ],
    "//conditions:default": [],
})

ABSEIL_DEFINES = [
    "ABSL_FORCE_ALIGNED_ACCESS",
]

BOOST_DEFINES = [
    "BOOST_ENABLE_ASSERT_DEBUG_HANDLER",
    # TODO: Ideally, we could not set this define in C++20 builds, but at least
    # our current Xcode 12 doesn't offer std::atomic_ref, so we cannot.
    "BOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF",
    "BOOST_LOG_NO_SHORTHAND_NAMES",
    "BOOST_LOG_USE_NATIVE_SYSLOG",
    "BOOST_LOG_WITHOUT_THREAD_ATTR",
    "BOOST_MATH_NO_LONG_DOUBLE_MATH_FUNCTIONS",
    "BOOST_SYSTEM_NO_DEPRECATED",
    "BOOST_THREAD_USES_DATETIME",
    "BOOST_THREAD_VERSION=5",
] + select({
    "//bazel/config:linkdynamic_not_shared_archive": ["BOOST_LOG_DYN_LINK"],
    "//conditions:default": [],
}) + select({
    "@platforms//os:windows": ["BOOST_ALL_NO_LIB"],
    "//conditions:default": [],
})

ENTERPRISE_DEFINES = select({
    "//bazel/config:build_enterprise_enabled": ["MONGO_ENTERPRISE_VERSION=1"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:enterprise_feature_audit_enabled": ["MONGO_ENTERPRISE_AUDIT=1"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:enterprise_feature_encryptdb_enabled": ["MONGO_ENTERPRISE_ENCRYPTDB=1"],
    "//conditions:default": [],
})

# Fortify only possibly makes sense on POSIX systems, and we know that clang is
# not a valid combination:
# http://lists.llvm.org/pipermail/cfe-dev/2015-November/045852.html
GCC_OPT_DEFINES = select({
    "//bazel/config:gcc_opt": ["_FORTIFY_SOURCE=2"],
    "//conditions:default": [],
})

# TODO(SERVER-54659): ASIO depends on std::result_of which was removed in C++ 20
LIBCXX_DEFINES = select({
    "//bazel/config:use_libcxx_required_settings": ["ASIO_HAS_STD_INVOKE_RESULT"],
    "//bazel/config:use_libcxx_disabled": [],
}, no_match_error = LIBCXX_ERROR_MESSAGE)

DEBUG_DEFINES = select({
    "//bazel/config:dbg_enabled": [],
    "//conditions:default": ["NDEBUG"],
})

PCRE2_DEFINES = ["PCRE2_STATIC"]

SAFEINT_DEFINES = ["SAFEINT_USE_INTRINSICS=0"]

# Unfortunately, abseil requires that we make these macros (this, and THREAD_
# and UNDEFINED_BEHAVIOR_ below) set, because apparently it is too hard to query
# the running compiler. We do this unconditionally because abseil is basically
# pervasive via the 'base' library.
ADDRESS_SANITIZER_DEFINES = select({
    "//bazel/config:sanitize_address_required_settings": ["ADDRESS_SANITIZER"],
    "//bazel/config:asan_disabled": [],
}, no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE)

THREAD_SANITIZER_DEFINES = select({
    "//bazel/config:sanitize_thread_required_settings": ["THREAD_SANITIZER"],
    "//bazel/config:tsan_disabled": [],
}, no_match_error = THREAD_SANITIZER_ERROR_MESSAGE)

UNDEFINED_SANITIZER_DEFINES = select({
    "//bazel/config:ubsan_enabled": ["UNDEFINED_BEHAVIOR_SANITIZER"],
    "//bazel/config:ubsan_disabled": [],
})

GLIBCXX_DEBUG_DEFINES = select({
    ("//bazel/config:use_glibcxx_debug_required_settings"): ["_GLIBCXX_DEBUG"],
    ("//bazel/config:use_glibcxx_debug_disabled"): [],
}, no_match_error = GLIBCXX_DEBUG_ERROR_MESSAGE)

TCMALLOC_DEFINES = select({
    "//bazel/config:tcmalloc_google_enabled": ["ABSL_ALLOCATOR_NOTHROW"],
    "//conditions:default": [],
})

MONGO_GLOBAL_DEFINES = (
    DEBUG_DEFINES +
    LIBCXX_DEFINES +
    ADDRESS_SANITIZER_DEFINES +
    THREAD_SANITIZER_DEFINES +
    UNDEFINED_SANITIZER_DEFINES +
    GLIBCXX_DEBUG_DEFINES +
    WINDOWS_DEFINES +
    MACOS_DEFINES +
    TCMALLOC_DEFINES +
    LINUX_DEFINES +
    GCC_OPT_DEFINES +
    BOOST_DEFINES +
    ABSEIL_DEFINES +
    PCRE2_DEFINES +
    SAFEINT_DEFINES +
    ENTERPRISE_DEFINES
)
