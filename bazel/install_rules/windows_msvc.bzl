def find_windows_msvc(ctx):
    command = [
        "vswhere",
        "-latest",
        "-property",
        "installationPath",
    ]
    result = ctx.execute(command, quiet = True)
    if result.return_code == 0:
        installation_path = result.stdout.strip()
        ctx.symlink(installation_path + "/VC/Redist/MSVC", "msvc")
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
        fail("Failed to locate Visual Studio using vswhere: " + result.stderr + ". Make sure you are on Windows and have Visual Studio installed.")
    return None

windows_msvc = repository_rule(
    implementation = find_windows_msvc,
    environ = ["VCINSTALLDIR"],
)
