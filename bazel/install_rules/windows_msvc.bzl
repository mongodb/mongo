def find_windows_msvc(ctx):
    vc_dir = ctx.os.environ.get("VCINSTALLDIR")
    if vc_dir:
        ctx.symlink(vc_dir + "Redist/MSVC", "msvc")
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
""",
        )
    else:
        fail("Environment variable VCINSTALLDIR must be set to find local msvc.")
    return None

windows_msvc = repository_rule(
    implementation = find_windows_msvc,
    environ = ["VCINSTALLDIR"],
)
