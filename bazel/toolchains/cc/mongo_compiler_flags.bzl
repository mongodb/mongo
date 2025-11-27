"""This file contains compiler flags that is specific to C++ compiling and linking."""

load(
    "//bazel/toolchains/cc/mongo_linux:mongo_compiler_flags.bzl",
    "MONGO_LINUX_CC_COPTS",
    "MONGO_LINUX_CC_LINKFLAGS",
)

# Only visible in the build system.
visibility([
    "//bazel",
    "//src/mongo/util",
])

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

MONGO_GLOBAL_COPTS = MONGO_LINUX_CC_COPTS

def get_copts(name, package_name, copts = []):
    copts = MONGO_GLOBAL_COPTS + \
            copts + \
            force_includes_copt(package_name, name)
    return copts

MONGO_GLOBAL_LINKFLAGS = MONGO_LINUX_CC_LINKFLAGS

def get_linkopts(package_name, linkopts = []):
    return MONGO_GLOBAL_LINKFLAGS + linkopts
