load("//bazel/toolchains/cc/mongo_linux:mongo_gdb_version_v5.bzl", "TOOLCHAIN_MAP_V5")
load("//bazel:utils.bzl", "generate_noop_toolchain", "get_toolchain_subs", "retry_download_and_extract", "write_python_pyc_cache_prefix_customization")

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

    gdbhome = external + "/gdb_" + ctx.attr.version + "/stow/gdb-" + ctx.attr.version
    gdb_prefix = external + "/gdb_" + ctx.attr.version + "/" + ctx.attr.version

    mongodb_toolchain_path = external + "/mongo_toolchain_" + ctx.attr.version
    stdlib_pp_dir = mongodb_toolchain_path + "/stow/gcc-" + ctx.attr.version + "/share"
    readelf = mongodb_toolchain_path + "/" + ctx.attr.version + "/bin/llvm-readelf"

    if "amazon_linux_2" == distro:
        # our toolchain python version requires newer openssl, which is not available on AL2
        # so we can use pretty printers on AL2
        python_env = "{}"
        wrapper_python_setup = ""
    else:
        # here we only have one dependency for our pretty printers to run. It must be installed into the python
        # that gdb was built with. We use pip since this is a single dependency that we own.
        #
        # NOTE: The bundled python is dynamically linked against libpython. Ensure it can locate its shared
        # library during repository fetch (and later at runtime) by providing LD_LIBRARY_PATH.
        python_lib_path = ":".join([
            pythonhome + "/lib",
            pythonhome + "/lib64",
        ])
        python_execute_env = {
            "PYTHONHOME": pythonhome,
            "LD_LIBRARY_PATH": python_lib_path,
            "PYTHONDONTWRITEBYTECODE": "1",
        }

        # Ensure the bundled Python does not write .pyc files into the toolchain/runfiles tree.
        write_python_pyc_cache_prefix_customization(
            ctx,
            "stow/python3-{version}/lib/python{pyver}/site-packages/sitecustomize.py".format(
                version = ctx.attr.version,
                pyver = python3_version,
            ),
        )

        result = ctx.execute([
            pythonhome + "/bin/python3",
            "-m",
            "pip",
            "install",
            "pymongo==4.12.0",
            "--target=" + pythonhome + "/lib/python" + python3_version + "/site-packages",
        ], environment = python_execute_env)
        if result.return_code != 0:
            if ctx.getenv("CI"):
                fail("Failed to install python module:\n" + result.stdout + "\n" + result.stderr)
            else:
                print("Failed to install python module:\n" + result.stdout + "\n" + result.stderr)
                print("This means some pretty printer functions will not work while debugging.")

        python_env = """{
        "PYTHONPATH": "%s/lib/python3.10",
        "PYTHONHOME": "%s",
        "LD_LIBRARY_PATH": "%s",
        "MONGO_GDB_PP_DIR": "%s",
        "MONGO_GDB_READELF": "%s",
    }""" % (pythonhome, pythonhome, python_lib_path, stdlib_pp_dir, readelf)

        # The wrapper scripts must also export these so gdb can load its python runtime (and pretty printers)
        # when invoked via bazel run/test.
        wrapper_python_setup = """
PYTHONHOME="${RUNFILES_WORKING_DIRECTORY}/../gdb_%s/stow/python3-%s"
export PYTHONHOME
export PYTHONPATH="${PYTHONHOME}/lib/python%s:${PYTHONPATH:-}"
export LD_LIBRARY_PATH="${PYTHONHOME}/lib:${PYTHONHOME}/lib64:${LD_LIBRARY_PATH:-}"
""" % (ctx.attr.version, ctx.attr.version, python3_version)

    # GDB itself is dynamically linked against its own runtime libraries (e.g. libopcodes). Ensure those are
    # available in runfiles and on the loader path regardless of platform.
    wrapper_gdb_setup = """
GDB_PREFIX="${RUNFILES_WORKING_DIRECTORY}/../gdb_%s/%s"
GDBHOME="${RUNFILES_WORKING_DIRECTORY}/../gdb_%s/stow/gdb-%s"
export LD_LIBRARY_PATH="${GDB_PREFIX}/lib:${GDBHOME}/lib:${LD_LIBRARY_PATH:-}"
""" % (ctx.attr.version, ctx.attr.version, ctx.attr.version, ctx.attr.version)

    ctx.file(
        "BUILD.bazel",
        """
filegroup(
    name = "python_runtime",
    srcs = glob(["stow/python3-%s/**"]),
    visibility = ["//visibility:private"],
)

filegroup(
    name = "gdb_runtime",
    srcs = glob([
        "%s/lib/**",
        "stow/gdb-%s/**",
    ]),
    visibility = ["//visibility:private"],
)

sh_binary(
    name = "gdb",
    srcs = ["working_dir_gdb.sh"],
    data = [
        "%s/bin/gdb",
        ":gdb_runtime",
        ":python_runtime",
    ],
    env = %s,
    visibility = ["//visibility:public"],
)

sh_binary(
    name = "gdbserver",
    srcs = ["working_dir_gdbserver.sh"],
    data = [
        "gdb",
        "%s/bin/gdbserver",
        ":gdb_runtime",
        ":python_runtime",
    ],
    visibility = ["//visibility:public"],
)
""" % (ctx.attr.version, ctx.attr.version, ctx.attr.version, ctx.attr.version, python_env, ctx.attr.version),
    )

    ctx.file(
        "working_dir_gdb.sh",
        """
#!/bin/bash

set -e

RUNFILES_WORKING_DIRECTORY="$(pwd)"

if [ -z $BUILD_WORKING_DIRECTORY ]; then
    echo "ERROR: BUILD_WORKING_DIRECTORY was not set, was this run from bazel?"
    exit 1
fi

cd $BUILD_WORKING_DIRECTORY
%s
%s
${RUNFILES_WORKING_DIRECTORY}/../gdb_%s/%s/bin/gdb -iex "set auto-load safe-path %s/.gdbinit" "${@:1}"
""" % (wrapper_gdb_setup, wrapper_python_setup, ctx.attr.version, ctx.attr.version, str(ctx.workspace_root)),
    )

    ctx.file(
        "working_dir_gdbserver.sh",
        """
#!/bin/bash

set -e

RUNFILES_WORKING_DIRECTORY="$(pwd)"

if [ -z $BUILD_WORKING_DIRECTORY ]; then
    echo "ERROR: BUILD_WORKING_DIRECTORY was not set, was this run from bazel?"
    exit 1
fi

cd $BUILD_WORKING_DIRECTORY

# RUNTEST_PRESERVE_CWD forces us to reconstruct the binary path
original_args="${@:1}"
%s
%s
${RUNFILES_WORKING_DIRECTORY}/external/gdb_%s/%s/bin/gdbserver localhost:1234 ${TEST_SRCDIR}/_main/${original_args[0]} "${@:2}"
""" % (wrapper_gdb_setup, wrapper_python_setup, ctx.attr.version, ctx.attr.version),
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
        name = "gdbserver",
        actual = "@gdb_v5//:gdbserver",
    )

    native.alias(
        name = "gdb_v5",
        actual = "@gdb_v5//:gdb",
    )
    native.alias(
        name = "gdbserver_v5",
        actual = "@gdb_v5//:gdbserver",
    )
