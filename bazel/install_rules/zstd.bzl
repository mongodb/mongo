def _zstd(ctx):
    zstd_bin = ctx.which("zstd")

    if zstd_bin:
        ctx.symlink(zstd_bin, "zstd")
        ctx.file(
            "BUILD.bazel",
            """
package(default_visibility = ["//visibility:public"])

config_setting(
    name = "zstd_tool_available",
    constraint_values = [
        "@platforms//os:%s",
    ]
)

sh_binary(
    name = "bin",
    srcs = ["zstd"],
)
""" % (ctx.os.name),
        )

    else:
        ctx.file(
            "BUILD.bazel",
            """
package(default_visibility = ["//visibility:public"])

constraint_value(
    name = "not_set",
    constraint_setting = "@platforms//cpu",
)

config_setting(
    name = "zstd_tool_available",
    constraint_values = [
        ":not_set",
    ]
)

sh_binary(
    name = "bin",
    srcs = [],
)
""",
        )

setup_zstd = repository_rule(
    implementation = _zstd,
)
