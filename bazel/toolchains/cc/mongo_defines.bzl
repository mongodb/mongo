"""This module provides a list of defines that is passed in to compiling that is agnostic to the OS.
"""

load(
    "//bazel/toolchains/cc:mongo_errors.bzl",
    "GLIBCXX_DEBUG_ERROR_MESSAGE",
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
    ADDRESS_SANITIZER_DEFINES +
    THREAD_SANITIZER_DEFINES +
    UNDEFINED_SANITIZER_DEFINES +
    GLIBCXX_DEBUG_DEFINES +
    TCMALLOC_DEFINES +
    GCC_OPT_DEFINES +
    BOOST_DEFINES +
    ABSEIL_DEFINES +
    PCRE2_DEFINES +
    SAFEINT_DEFINES +
    ENTERPRISE_DEFINES
)
