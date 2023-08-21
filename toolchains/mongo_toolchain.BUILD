# This file exists to describe "mongo_toolchain", the http_archive defined in WORKSPACE.bazel

load("@bazel_tools//tools/cpp:unix_cc_toolchain_config.bzl", "cc_toolchain_config")

package(default_visibility = ["//visibility:public"])

# Establish a "platform" target so Bazel can pick the right platform for building:
platform(
    name = "platform",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
        "@bazel_tools//tools/cpp:gcc",
    ],
)

# Helper target for the toolchain (see below):
filegroup(
    name = "all",
    srcs = glob(["**/*"]),
)

# A note on toolchains: this is complicated! Bazel requires multiple layers of indirection.
#
# In this case, we have:
# cc_gcc_toolchain_config (of type cc_toolchain_config)
# referenced by: cc_mongo_toolchain (of type cc_toolchain)
# referenced by: mongo_toolchain (of type toolchain)
# referenced by: toolchain_suite (of type cc_toolchain_suite)

cc_toolchain_config(
    name = "cc_gcc_toolchain_config",
    abi_libc_version = "unknown",
    abi_version = "unknown",
    compile_flags = [
        "--verbose",
        "-std=c++20",
        "-nostdinc++",
        # These flags are necessary to get system includes properly available for compilation:
        "-isystem",
        "external/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0/include",
        "-isystem",
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0",
        "-isystem",
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/aarch64-mongodb-linux",
        # These flags are necessary for the link step to work remotely:
        "-Bexternal/mongo_toolchain/v4/bin",
        "-Bexternal/mongo_toolchain/v4/lib",
        "-Bexternal/mongo_toolchain/stow/gcc-v4/libexec/gcc/aarch64-mongodb-linux/11.3.0",
    ],
    compiler = "gcc",
    cpu = "arm64",
    cxx_builtin_include_directories = [
        "/usr/include",
    ],
    host_system_name = "local",
    link_flags = [
        # These flags are necessary for the link step to work remotely:
        "-nostdinc++",
        "-Lexternal/mongo_toolchain/v4/lib",
        "-Lexternal/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0",
        "-Bexternal/mongo_toolchain/stow/gcc-v4/libexec/gcc/aarch64-mongodb-linux/11.3.0",
        "-Bexternal/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0",
    ],
    target_libc = "unknown",
    target_system_name = "local",
    tool_paths = {
        "gcc": "v4/bin/g++",
        "cpp": "v4/bin/cpp",
        "ar": "v4/bin/ar",
        "nm": "v4/bin/nm",
        "ld": "v4/bin/ld",
        "as": "v4/bin/as",
        "objcopy": "v4/bin/objcopy",
        "objdump": "v4/bin/objdump",
        "gcov": "v4/bin/gcov",
        "strip": "v4/bin/strip",
        "llvm-cov": "/bin/false",  # /bin/false = we're not using llvm-cov
    },
    toolchain_identifier = "mongo_v4_gcc",
)

cc_toolchain(
    name = "cc_mongo_toolchain",
    all_files = ":all",
    ar_files = ":all",
    compiler_files = ":all",
    dwp_files = ":all",
    linker_files = ":all",
    objcopy_files = ":all",
    strip_files = ":all",
    toolchain_config = ":cc_gcc_toolchain_config",
)

toolchain(
    name = "mongo_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:arm64",
        "@bazel_tools//tools/cpp:gcc",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:arm64",
    ],
    toolchain = ":cc_mongo_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)

cc_toolchain_suite(
    name = "toolchain_suite",
    toolchains = {
        "k8": ":cc_mongo_toolchain",
        "aarch64": ":cc_mongo_toolchain",
    },
)
