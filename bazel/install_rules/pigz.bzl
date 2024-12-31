def _pigz(ctx):
    pigz_bin = ctx.which("pigz")

    if pigz_bin:
        ctx.symlink(pigz_bin, "pigz")
        ctx.file(
            "BUILD.bazel",
            """
package(default_visibility = ["//visibility:public"])

config_setting(
    name = "pigz_tool_available",
    constraint_values = [
        "@platforms//os:%s",
    ]
)

sh_binary(
    name = "bin",
    srcs = ["pigz"],
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
    name = "pigz_tool_available",
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

setup_pigz = repository_rule(
    implementation = _pigz,
)
