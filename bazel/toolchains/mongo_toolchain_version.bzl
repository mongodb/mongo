load("//bazel/toolchains:mongo_toolchain_version_v4.bzl", "TOOLCHAIN_MAP_V4")
load("//bazel/toolchains:mongo_toolchain_version_v5.bzl", "TOOLCHAIN_MAP_V5")

TOOLCHAIN_MAP = {
    "v4": TOOLCHAIN_MAP_V4,
    "v5": TOOLCHAIN_MAP_V5,
}
