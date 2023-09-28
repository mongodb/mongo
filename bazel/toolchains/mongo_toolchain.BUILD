# This file exists to describe "mongo_toolchain", the http_archive defined in WORKSPACE.bazel

load("@bazel_tools//tools/cpp:unix_cc_toolchain_config.bzl", "cc_toolchain_config")

package(default_visibility = ["//visibility:public"])

# Establish a "platform" target so Bazel can pick the right platform for building:
platform(
    name = "platform",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:arm64",
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

# Create a cc_toolchain_config for both 'gcc' and 'clang' compilers:
[
    cc_toolchain_config(
        name = "cc_" + compiler_name + "_toolchain_config",  # Note: I'd prefer to use an f-string but Bazel doesn't support that feature
        abi_libc_version = "unknown",
        abi_version = "unknown",
        compile_flags = [
            "--verbose",
            "-std=c++20",
            "-nostdinc++",
            # These isystems make sure that toolchain includes are used in place of any remote system
            "-isystem",
            "external/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0/include",
            "-isystem",
            "external/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0/include-fixed",
            "-isystem",
            "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0",
            "-isystem",
            "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/aarch64-mongodb-linux",
            # Make sure that the toolchain binaries are available
            "-Bexternal/mongo_toolchain/v4/bin",
            "-Bexternal/mongo_toolchain/v4/lib",
            "-Bexternal/mongo_toolchain/stow/gcc-v4/libexec/gcc/aarch64-mongodb-linux/11.3.0",
        ],
        compiler = compiler_name,
        cpu = "arm64",
        cxx_builtin_include_directories = [
            "/usr/include",
            # See undocumented %package() syntax: https://cs.opensource.google/bazel/bazel/+/6d448136d13ddab92da8bb29ea6e8387821369d9:src/main/java/com/google/devtools/build/lib/rules/cpp/CcToolchainProviderHelper.java;l=309-329
            "%package(@mongo_toolchain//stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0/include)%",
            "%package(@mongo_toolchain//stow/gcc-v4/include/c++/11.3.0)%",
            "%package(@mongo_toolchain//stow/gcc-v4/include/c++/11.3.0/aarch64-mongodb-linux)%",
            "%package(@mongo_toolchain//stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0/include-fixed)%",
        ],
        unfiltered_compile_flags = [
            # Do not resolve our symlinked resource prefixes to real paths. This is required to
            # make includes resolve correctly.
            "-no-canonical-prefixes",
            # Replace compile timestamp-related macros for reproducible binaries with consistent hashes.
            "-Wno-builtin-macro-redefined",
            "-D__DATE__=\"OMITTED_FOR_HASH_CONSISTENCY\"",
            "-D__TIMESTAMP__=\"OMITTED_FOR_HASH_CONSISTENCY\"",
            "-D__TIME__=\"OMITTED_FOR_HASH_CONSISTENCY\"",
        ],
        host_system_name = "local",
        link_flags = [
            # Don't use remote system includes, only our toolchain includes
            "-nostdinc++",
            # Make sure that our toolchain libraries are used for linking
            "-Lexternal/mongo_toolchain/v4/lib",
            "-Lexternal/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0",
            "-Bexternal/mongo_toolchain/stow/gcc-v4/libexec/gcc/aarch64-mongodb-linux/11.3.0",
            "-Bexternal/mongo_toolchain/stow/gcc-v4/lib/gcc/aarch64-mongodb-linux/11.3.0",
        ],
        target_libc = "unknown",
        target_system_name = "local",
        tool_paths = {
            # Note: You might assume that the specification of `copmiler_name` (above) would be sufficient to make Bazel
            # use the correct binary. This is incorrect; Bazel appears to unconditionally use the `gcc` tool_path. As a result,
            # we have to conditionally set the value pointed to by `gcc`.
            "gcc": "v4/bin/" + compiler_binary,  # Note: I'd prefer to use an f-string but Bazel doesn't support that feature
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
        toolchain_identifier = "mongo_v4_" + compiler_name,  # Note: I'd prefer to use an f-string but Bazel doesn't support that feature
    )
    for compiler_name, compiler_binary in [
        ("clang", "clang++"),
        ("gcc", "g++"),
    ]
]

cc_toolchain(
    name = "cc_mongo_toolchain",
    all_files = ":all",
    ar_files = ":all",
    compiler_files = ":all",
    dwp_files = ":all",
    linker_files = ":all",
    objcopy_files = ":all",
    strip_files = ":all",
    toolchain_config = select({
        "@//bazel/config:compiler_type_clang": ":cc_clang_toolchain_config",
        "@//bazel/config:compiler_type_gcc": ":cc_gcc_toolchain_config",
    }),
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
