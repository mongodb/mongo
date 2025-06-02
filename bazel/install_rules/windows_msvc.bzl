load(
    "//bazel/toolchains/cc/mongo_windows:windows_cc_configure.bzl",
    "find_vc_path",
    "get_vc_redist_version",
)

def find_windows_msvc(ctx):
    vc_path = find_vc_path(ctx)
    if vc_path == None:
        fail("Failed to locate Visual Studio. Make sure you are on Windows and have Visual Studio installed.")

    redist_version = get_vc_redist_version(ctx)
    if redist_version == None:
        fail("Failed to locate a redistribution version from Visual Studio")

    ctx.symlink(vc_path + "/Redist/MSVC/" + redist_version, "msvc")
    ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "merge_modules",
    srcs = select({
        "@platforms//os:windows": glob(["**/*.msm"]),
        "//conditions:default": [],
    }),
)

filegroup(
    name = "vc_redist_x64",
    srcs = select({
        "@platforms//os:windows": glob(["**/vc_redist.x64.exe"]),
        "//conditions:default": [],
    }),
)
""",
    )
    return None

windows_msvc = repository_rule(
    environ = [
        "BAZEL_VC_FULL_VERSION",  # Force re-compute if the user changed the version of MS compiler.
        "MONGO_VC_REDIST_FULL_VERSION",  # Force re-compute if the user changed the VC Redistribution version.
    ],
    implementation = find_windows_msvc,
    configure = True,
    local = True,
)
