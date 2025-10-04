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
                      "@//bazel/config:not_windows",
    ],
)

constraint_value(
    name = "not_set",
    constraint_setting = "@platforms//cpu",
)

config_setting(
    name = "zstd_tool_not_available",
    constraint_values = [
        ":not_set",
    ],
)

sh_binary(
    name = "bin",
    srcs = ["zstd"],
)
""",
        )
    else:
        ctx.file(
            "BUILD.bazel",
            """
load("@bazel_skylib//lib:selects.bzl", "selects")
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

selects.config_setting_group(
    name = "zstd_tool_not_available",
    match_any = [
        ":always_true_0",
        ":always_true_1",
    ],
)
config_setting(
    name = "always_true_0",
    values = {"stamp": "0"},
)
config_setting(
    name = "always_true_1",
    values = {"stamp": "1"},
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
