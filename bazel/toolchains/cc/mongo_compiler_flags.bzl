"""This file contains compiler flags that is specific to C++ compiling and linking."""

load(
    "//bazel/toolchains/cc/mongo_linux:mongo_compiler_flags.bzl",
    "MONGO_LINUX_CC_COPTS",
    "MONGO_LINUX_CC_LINKFLAGS",
)
load(
    "//bazel/toolchains/cc/mongo_windows:mongo_compiler_flags.bzl",
    "MONGO_WIN_CC_COPTS",
    "MONGO_WIN_CC_LINKFLAGS",
    "WINDOWS_MULTITHREAD_RUNTIME_COPTS",
)

# Only visible in the build system.
visibility([
    "//bazel",
    "//src/mongo/util",
])

# Used as both link flags and copts
# Suppress the function sanitizer check for third party libraries, because:
#
# - mongod (a C++ binary) links in WiredTiger (a C library)
# - If/when mongod--built under ubsan--fails, the sanitizer will by
#   default analyze the failed execution for undefined behavior related to
#   function pointer usage. See:
#   https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html#available-checks
# - When this happens, the sanitizer will attempt to dynamically load to perform
#   the analysis.
# - However, since WT was built as a C library, is not linked with the function
#   sanitizer library symbols despite its C++ dependencies referencing them.
# - This will cause the sanitizer itself to fail, resulting in debug information
#   being unavailable.
# - So by suppressing the function ubsan check, we won't reference symbols
#   defined in the unavailable ubsan function sanitier library and will get
#   useful debugging information.
UBSAN_OPTS_THIRD_PARTY = select({
    "//bazel/config:sanitize_undefined_dynamic_link_settings": [
        "-fno-sanitize=function",
    ],
    "//conditions:default": [],
})

def force_includes_copt(package_name, name):
    if package_name.startswith("src/mongo"):
        basic_h = "mongo/platform/basic.h"
        return select({
            "@platforms//os:windows": ["/FI" + basic_h],
            "//conditions:default": ["-include", basic_h],
        })

    if name in ["scripting", "scripting_mozjs_test", "encrypted_dbclient"]:
        return select({
            "//bazel/config:linux_aarch64": ["-include", "third_party/mozjs/platform/aarch64/linux/build/js-config.h"],
            "//bazel/config:linux_ppc64le": ["-include", "third_party/mozjs/platform/ppc64le/linux/build/js-config.h"],
            "//bazel/config:linux_s390x": ["-include", "third_party/mozjs/platform/s390x/linux/build/js-config.h"],
            "//bazel/config:linux_x86_64": ["-include", "third_party/mozjs/platform/x86_64/linux/build/js-config.h"],
            "//bazel/config:macos_aarch64": ["-include", "third_party/mozjs/platform/aarch64/macOS/build/js-config.h"],
            "//bazel/config:macos_x86_64": ["-include", "third_party/mozjs/platform/x86_64/macOS/build/js-config.h"],
            "//bazel/config:windows_x86_64": ["/FI" + "third_party/mozjs/platform/x86_64/windows/build/js-config.h"],
        })

    return []

# TODO(SERVER-103006): Stop including this flag when ASP is able to upgrade mongoc and mongocxx
STREAMS_THIRD_PARTY_DIR = "src/mongo/db/modules/enterprise/src/streams/third_party"

def package_specific_copt(package_name):
    if package_name.startswith("src/third_party") or package_name.startswith(STREAMS_THIRD_PARTY_DIR):
        return UBSAN_OPTS_THIRD_PARTY
    return []

def package_specific_linkflag(package_name):
    if package_name.startswith("src/third_party") or package_name.startswith(STREAMS_THIRD_PARTY_DIR):
        return UBSAN_OPTS_THIRD_PARTY
    return []

MONGO_GLOBAL_COPTS = MONGO_LINUX_CC_COPTS + MONGO_WIN_CC_COPTS

def get_copts(name, package_name, copts = [], skip_windows_crt_flags = False):
    copts = MONGO_GLOBAL_COPTS + \
            package_specific_copt(package_name) + \
            copts + \
            force_includes_copt(package_name, name)
    if not skip_windows_crt_flags:
        copts = copts + WINDOWS_MULTITHREAD_RUNTIME_COPTS
    return copts

MONGO_GLOBAL_LINKFLAGS = MONGO_LINUX_CC_LINKFLAGS + MONGO_WIN_CC_LINKFLAGS

def get_linkopts(package_name, linkopts = []):
    return MONGO_GLOBAL_LINKFLAGS + package_specific_linkflag(package_name) + linkopts
