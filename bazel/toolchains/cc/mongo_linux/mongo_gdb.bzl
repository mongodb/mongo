load("//bazel/toolchains/cc/mongo_linux:mongo_gdb_version_v5.bzl", "TOOLCHAIN_MAP_V5")
load("//bazel:utils.bzl", "generate_noop_toolchain", "get_toolchain_subs", "retry_download_and_extract")

def _gdb_download(ctx):
    distro, arch, substitutions = get_toolchain_subs(ctx)

    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = arch)

    if toolchain_key in TOOLCHAIN_MAP_V5:
        toolchain_info = TOOLCHAIN_MAP_V5[toolchain_key]
        urls = toolchain_info["url"]
        sha = toolchain_info["sha"]

        ctx.report_progress("downloading {} gdb: {}".format(toolchain_key, urls))
        retry_download_and_extract(
            ctx = ctx,
            tries = 5,
            url = urls,
            sha256 = sha,
        )

        ctx.report_progress("generating gdb " + ctx.attr.version + " build file")
        pythonhome = str(ctx.workspace_root) + "/bazel-mongo/external/gdb/stow/python3-" + ctx.attr.version
        ctx.file(
            "BUILD.bazel",
            """
sh_binary(
    name = "gdb",
    srcs = ["%s/bin/gdb"],
    env = {"PYTHONPATH": "%s/lib/python3.10", "PYTHONHOME": "%s"},
)
""" % (ctx.attr.version, pythonhome, pythonhome),
        )

    else:
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("Mongo gdb " + ctx.attr.version + " not supported on this platform. Platform key not found: " + toolchain_key)

    return None

gdb_download = repository_rule(
    implementation = _gdb_download,
    attrs = {
        "os": attr.string(
            values = ["macos", "linux", "windows"],
            doc = "Host operating system.",
        ),
        "arch": attr.string(
            values = ["amd64", "aarch64", "amd64", "x86_64", "ppc64le", "s390x"],
            doc = "Host architecture.",
        ),
        "version": attr.string(
            values = ["v4", "v5"],
            doc = "Mongodbtoolchain version.",
            mandatory = True,
        ),
    },
)
