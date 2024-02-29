# This file exists to describe "mongo_toolchain", the http_archive defined in WORKSPACE.bazel

load("@//bazel/toolchains:mongo_cc_toolchain_config.bzl", "mongo_cc_toolchain_config")

package(default_visibility = ["//visibility:public"])

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

COMMON_LINK_FLAGS = [
    # Make sure that our toolchain libraries are used for linking
    "-Lexternal/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0",
    "-Lexternal/mongo_toolchain/v4/lib",
    "-Lexternal/mongo_toolchain/v4/lib64",
    "-Bexternal/mongo_toolchain/stow/gcc-v4/libexec/gcc/{arch}-mongodb-linux/11.3.0",
    "-Bexternal/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0",
    "-Bexternal/mongo_toolchain/stow/llvm-v4/bin",
    # Use the host system's glibc dynamic libraries
    "-B/lib/{arch}-linux-gnu",
    "-B/usr/lib/{arch}-linux-gnu",
]

COMMON_INCLUDE_DIRECTORIES = [
    "/usr/include",
    # See undocumented %package() syntax: https://cs.opensource.google/bazel/bazel/+/6d448136d13ddab92da8bb29ea6e8387821369d9:src/main/java/com/google/devtools/build/lib/rules/cpp/CcToolchainProviderHelper.java;l=309-329
    "%package(@mongo_toolchain//stow/gcc-v4/include/c++/11.3.0)%",
    "%package(@mongo_toolchain//stow/gcc-v4/include/c++/11.3.0/{arch}-mongodb-linux)%",
]

mongo_cc_toolchain_config(
    name = "cc_gcc_toolchain_config",
    toolchain_identifier = "gcc_v4_toolchain",
    compiler = "gcc",
    cpu = "{platforms_arch}",
    verbose = True,
    includes = [
        # These isystems make sure that toolchain includes are used in place of any remote system
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0",
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/{arch}-mongodb-linux",
        "external/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0/include",
        "external/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0/include-fixed",
        # Use the host system's glibc headers
        "/usr/include/{arch}-linux-gnu",
    ],
    bin_dirs = [
        # Make sure that the toolchain binaries are available
        "external/mongo_toolchain/v4/bin",
        "external/mongo_toolchain/stow/gcc-v4/libexec/gcc/{arch}-mongodb-linux/11.3.0",
    ],
    cxx_builtin_include_directories = COMMON_INCLUDE_DIRECTORIES + [
        # See undocumented %package() syntax: https://cs.opensource.google/bazel/bazel/+/6d448136d13ddab92da8bb29ea6e8387821369d9:src/main/java/com/google/devtools/build/lib/rules/cpp/CcToolchainProviderHelper.java;l=309-329
        "%package(@mongo_toolchain//stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0/include)%",
        "%package(@mongo_toolchain//stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0/include-fixed)%",
    ],
    extra_ldflags = COMMON_LINK_FLAGS,
    tool_paths = {
        # Note: You might assume that the specification of `compiler_name` (above) would be sufficient to make Bazel
        # use the correct binary. This is incorrect; Bazel appears to unconditionally use the `gcc` tool_path. As a result,
        # we have to conditionally set the value pointed to by `gcc`.
        "gcc": "v4/bin/gcc",
        "g++": "v4/bin/g++",
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
)

mongo_cc_toolchain_config(
    name = "cc_clang_toolchain_config",
    toolchain_identifier = "clang_v4_toolchain",
    compiler = "clang",
    cpu = "{platforms_arch}",
    verbose = True,
    includes = [
        # These isystems make sure that toolchain includes are used in place of any remote system
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0",
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/{arch}-mongodb-linux",
        "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/backward",
        "external/mongo_toolchain/stow/llvm-v4/lib/clang/12.0.1/include",
        # Use the host system's glibc headers
        "/usr/include/{arch}-linux-gnu",
    ],
    bin_dirs = [
        # Make sure that the toolchain binaries are available
        "external/mongo_toolchain/v4/bin",
        "external/mongo_toolchain/stow/gcc-v4/libexec/gcc/{arch}-mongodb-linux/11.3.0",
    ],
    cxx_builtin_include_directories = COMMON_INCLUDE_DIRECTORIES + [
        # See undocumented %package() syntax: https://cs.opensource.google/bazel/bazel/+/6d448136d13ddab92da8bb29ea6e8387821369d9:src/main/java/com/google/devtools/build/lib/rules/cpp/CcToolchainProviderHelper.java;l=309-329
        "%package(@mongo_toolchain//stow/gcc-v4/include/c++/11.3.0/backward)%",
        "%package(@mongo_toolchain//stow/llvm-v4/lib/clang/12.0.1/include)%",
    ],
    extra_ldflags = COMMON_LINK_FLAGS + [
        # Make sure that our toolchain libraries are used for linking
        "-Bexternal/mongo_toolchain/stow/llvm-v4/lib/clang/12.0.1",
    ],
    tool_paths = {
        # Note: You might assume that the specification of `compiler_name` (above) would be sufficient to make Bazel
        # use the correct binary. This is incorrect; Bazel appears to unconditionally use the `gcc` tool_path. As a result,
        # we have to conditionally set the value pointed to by `gcc`.
        # TODO(SERVER-87211): The two lines below are using the absolute path to help clang find the sanitizer .a
        # files. Switch these to the v4/bin/* paths once EngFlow fixes the issue where symlinks are fully resolved
        # when copied to the remote execution system.
        "gcc": "stow/llvm-v4/bin/clang",
        "g++": "stow/llvm-v4/bin/clang++",
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
    toolchain_config = select({
        "@//bazel/config:compiler_type_clang": ":cc_clang_toolchain_config",
        "@//bazel/config:compiler_type_gcc": ":cc_gcc_toolchain_config",
    }),
)

toolchain(
    name = "mongo_toolchain",
    toolchain = ":cc_mongo_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{platforms_arch}",
        "@bazel_tools//tools/cpp:gcc",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{platforms_arch}",
    ],
)

cc_toolchain_suite(
    name = "toolchain_suite",
    toolchains = {
        "{bazel_toolchain_cpu}": ":cc_mongo_toolchain",
    },
)
