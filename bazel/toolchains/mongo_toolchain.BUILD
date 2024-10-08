# This file exists to describe "mongo_toolchain", the http_archive defined in WORKSPACE.bazel

load("@//bazel/toolchains:mongo_cc_toolchain_config.bzl", "mongo_cc_toolchain_config")
load("@mongo_toolchain//:mongo_toolchain_flags.bzl", "COMMON_LINK_FLAGS", "COMMON_BUILTIN_INCLUDE_DIRECTORIES", "COMMON_INCLUDE_DIRECTORIES", "COMMON_BINDIRS", "GCC_INCLUDE_DIRS", "CLANG_INCLUDE_DIRS")

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



mongo_cc_toolchain_config(
    name = "cc_gcc_toolchain_config",
    bin_dirs = COMMON_BINDIRS,
    compiler = "gcc",
    cpu = "{platforms_arch}",
    cxx_builtin_include_directories = COMMON_BUILTIN_INCLUDE_DIRECTORIES,
    extra_ldflags = ["-L"+flag for flag in COMMON_LINK_FLAGS],
    includes = GCC_INCLUDE_DIRS + COMMON_INCLUDE_DIRECTORIES + COMMON_BUILTIN_INCLUDE_DIRECTORIES,
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
        "objcopy": "v4/bin/llvm-objcopy",
        "objdump": "v4/bin/objdump",
        "gcov": "v4/bin/gcov",
        "strip": "v4/bin/strip",
        "llvm-cov": "/bin/false",  # /bin/false = we're not using llvm-cov
    },
    toolchain_identifier = "gcc_v4_toolchain",
    verbose = True,
)

mongo_cc_toolchain_config(
    name = "cc_clang_toolchain_config",
    bin_dirs = COMMON_BINDIRS,
    compiler = "clang",
    cpu = "{platforms_arch}",
    cxx_builtin_include_directories = COMMON_BUILTIN_INCLUDE_DIRECTORIES,
    extra_ldflags = ["-L"+flag for flag in COMMON_LINK_FLAGS],
    includes = CLANG_INCLUDE_DIRS + COMMON_INCLUDE_DIRECTORIES + COMMON_BUILTIN_INCLUDE_DIRECTORIES,
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
        "objcopy": "v4/bin/llvm-objcopy",
        "objdump": "v4/bin/objdump",
        "gcov": "v4/bin/gcov",
        "strip": "v4/bin/strip",
        "llvm-cov": "/bin/false",  # /bin/false = we're not using llvm-cov
    },
    toolchain_identifier = "clang_v4_toolchain",
    verbose = True,
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
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{platforms_arch}",
        "@//bazel/platforms:use_mongo_toolchain",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{platforms_arch}",
        "@//bazel/platforms:use_mongo_toolchain",
    ],
    toolchain = ":cc_mongo_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)

# This file group makes it possible to set the clang-tidy configuration setting:
#   --@bazel_clang_tidy//:clang_tidy_executable=@mongo_toolchain//:clang_tidy
filegroup(
    name = "clang_tidy",
    srcs = [
        "v4/bin/clang-tidy",
    ],
)
