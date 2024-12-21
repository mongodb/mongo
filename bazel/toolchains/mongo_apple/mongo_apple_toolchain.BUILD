load("@build_bazel_apple_support//configs:platforms.bzl", "APPLE_PLATFORMS_CONSTRAINTS")
load("@//bazel/toolchains/mongo_apple:mongo_apple_toolchain.bzl", "get_supported_apple_archs")
package(default_visibility = ["//visibility:public"])

[
    toolchain(
        name = "mongo_apple_" + apple_arch + " _toolchain",
        exec_compatible_with = [
            "@platforms//os:macos",
            "@platforms//cpu:" + cpu,
        ],
        target_compatible_with = [
            "@platforms//os:macos",
            "@platforms//cpu:" + cpu,
        ],
        toolchain = "@mongo_apple_toolchain_config//:cc-compiler-" + apple_arch,
        toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
    )
    for apple_arch, cpu in get_supported_apple_archs().items()
]