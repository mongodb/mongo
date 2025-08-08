load("//bazel/toolchains/cc/mongo_linux:mongo_gdb_version_v5.bzl", "TOOLCHAIN_MAP_V5")
load("//bazel:utils.bzl", "generate_noop_toolchain", "get_toolchain_subs", "retry_download_and_extract")

def _gdb_download(ctx):
    distro, arch, substitutions = get_toolchain_subs(ctx)
    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = arch)

    toolchain_info = None
    python3_version = None
    urls = None
    sha = None

    if ctx.attr.version == "v5":
        if toolchain_key in TOOLCHAIN_MAP_V5:
            python3_version = "3.10"
            toolchain_info = TOOLCHAIN_MAP_V5[toolchain_key]
            urls = toolchain_info["url"]
            sha = toolchain_info["sha"]

    if toolchain_info == None:
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("Mongo gdb " + ctx.attr.version + " not supported on this platform. Platform key not found: " + toolchain_key)
        return None

    ctx.report_progress("downloading {} gdb: {}".format(toolchain_key, urls))
    retry_download_and_extract(
        ctx = ctx,
        tries = 5,
        url = urls,
        sha256 = sha,
    )

    ctx.report_progress("generating gdb " + ctx.attr.version + " build file")

    external = str(ctx.path(".."))
    pythonhome = external + "/gdb_" + ctx.attr.version + "/stow/python3-" + ctx.attr.version

    mongodb_toolchain_path = external + "/mongo_toolchain_" + ctx.attr.version
    stdlib_pp_dir = mongodb_toolchain_path + "/stow/gcc-" + ctx.attr.version + "/share"
    readelf = mongodb_toolchain_path + "/" + ctx.attr.version + "/bin/llvm-readelf"

    if "amazon_linux_2" == distro:
        # our toolchain python version requires newer openssl, which is not available on AL2
        # so we can use pretty printers on AL2
        python_env = "{}"
    else:
        # here we only have one dependency for our pretty printers to run. It must be installed into the python
        # that gdb was built with. We use pip since this is a single dependency that we own.
        result = ctx.execute([
            pythonhome + "/bin/python3",
            "-m",
            "pip",
            "install",
            "pymongo==4.12.0",
            "--target=" + pythonhome + "/lib/python" + python3_version + "/site-packages",
        ])
        if result.return_code != 0:
            fail("Failed to install python module: " + result.stderr)

        python_env = """{
        "PYTHONPATH": "%s/lib/python3.10",
        "PYTHONHOME": "%s",
        "MONGO_GDB_PP_DIR": "%s",
        "MONGO_GDB_READELF": "%s",
    }""" % (pythonhome, pythonhome, stdlib_pp_dir, readelf)

    ctx.file(
        "BUILD.bazel",
        """
sh_binary(
    name = "gdb",
    srcs = ["working_dir.sh"],
    data = ["%s/bin/gdb"],
    env = %s,
    visibility = ["//visibility:public"],
)
""" % (ctx.attr.version, python_env),
    )

    ctx.file(
        "working_dir.sh",
        """
#!/bin/bash

set -e

RUNFILES_WORKING_DIRECTORY="$(pwd)"

if [ -z $BUILD_WORKING_DIRECTORY ]; then
    echo "ERROR: BUILD_WORKING_DIRECTORY was not set, was this run from bazel?"
    exit 1
fi

cd $BUILD_WORKING_DIRECTORY
${RUNFILES_WORKING_DIRECTORY}/../gdb_%s/%s/bin/gdb -iex "set auto-load safe-path %s/.gdbinit" "${@:1}"
""" % (ctx.attr.version, ctx.attr.version, str(ctx.workspace_root)),
    )

    return None

gdb_v5_download = repository_rule(
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
        "version": attr.string(),
        "mongo_toolchain": attr.label(
            allow_files = True,
        ),
    },
)

def setup_gdb_toolchains():
    gdb_v5_download(
        name = "gdb_v5",
        version = "v5",
        mongo_toolchain = "@mongo_toolchain_v5//:all",
    )

def setup_gdb_toolchain_aliases(name = "setup_toolchains"):
    # v5 is the default version we currently use, so we name it unversioned
    native.alias(
        name = "gdb",
        actual = "@gdb_v5//:gdb",
    )

    native.alias(
        name = "gdb_v5",
        actual = "@gdb_v5//:gdb",
    )
