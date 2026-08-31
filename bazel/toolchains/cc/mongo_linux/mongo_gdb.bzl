"""Repository rule for MongoDB's GDB wrapper/toolchain."""

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
            python3_version = "3.13"
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

    # Both repos are created by the `setup_mongo_toolchains` module extension,
    # so their directory (and runfiles) names are the canonical, mangled ones
    # — e.g. "_main~setup_mongo_toolchains~gdb_v5", not "gdb_v5". Derive them
    # rather than rebuilding "gdb_" + version, which no longer matches.
    gdb_repo = ctx.name
    toolchain_repo = ctx.attr.mongo_toolchain.workspace_name

    pythonhome = external + "/" + gdb_repo + "/stow/python313-" + ctx.attr.version

    gdb_prefix = external + "/" + gdb_repo + "/" + ctx.attr.version

    mongodb_toolchain_path = external + "/" + toolchain_repo
    stdlib_pp_dir = mongodb_toolchain_path + "/stow/gcc-" + ctx.attr.version + "/share"
    readelf = mongodb_toolchain_path + "/" + ctx.attr.version + "/bin/llvm-readelf"
    objcopy = mongodb_toolchain_path + "/" + ctx.attr.version + "/bin/llvm-objcopy"

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
            "stow/python313-{version}/lib/python{pyver}/site-packages/sitecustomize.py".format(
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
                ctx.report_progress("Failed to install python module; some pretty printer functions may not work while debugging.\nSTDOUT:\n{}\nSTDERR:\n{}".format(result.stdout, result.stderr))

        python_env = """{
        "PYTHONPATH": "%s/lib/python%s",
        "PYTHONHOME": "%s",
        "LD_LIBRARY_PATH": "%s",
        "MONGO_GDB_PP_DIR": "%s",
        "MONGO_GDB_READELF": "%s",
        "READELF": "%s",
        "OBJCOPY": "%s",
        "MONGO_GDB_OBJCOPY": "%s",
        "GDB": "%s/bin/gdb",
    }""" % (
            pythonhome,
            python3_version,
            pythonhome,
            python_lib_path,
            stdlib_pp_dir,
            readelf,
            readelf,
            objcopy,
            objcopy,
            gdb_prefix,
        )

        # The wrapper scripts must also export these so gdb can load its python runtime (and pretty printers)
        # when invoked via bazel run/test.
        wrapper_python_setup = """
PYTHONHOME="$(dirname "$(dirname "${GDBHOME}")")/stow/python313-%s"
export PYTHONHOME
export PYTHONPATH="${PYTHONHOME}/lib/python%s:${PYTHONPATH:-}"
export LD_LIBRARY_PATH="${PYTHONHOME}/lib:${PYTHONHOME}/lib64:${LD_LIBRARY_PATH:-}"
""" % (ctx.attr.version, python3_version)

    # GDB itself is dynamically linked against its own runtime libraries (e.g. libopcodes). Ensure those are
    # available in runfiles and on the loader path regardless of platform.
    wrapper_gdb_setup = """
GDB_PREFIX="${RUNFILES_WORKING_DIRECTORY}/../%s/%s"
GDBHOME="${RUNFILES_WORKING_DIRECTORY}/../%s/stow/gdb-%s"
export LD_LIBRARY_PATH="${GDB_PREFIX}/lib:${GDBHOME}/lib:${LD_LIBRARY_PATH:-}"
""" % (gdb_repo, ctx.attr.version, gdb_repo, ctx.attr.version)

    # Ensure GDB (and our in-GDB python helpers) use binutils that match the MongoDB toolchain.
    #
    # Some GDB operations call out to external binutils (e.g. readelf/objcopy). When invoked via
    # `bazel run`/`bazel test`, we want these to come from the same mongo toolchain version used
    # by the build, not the host OS.
    wrapper_binutils_setup = """
# Prefer resolving runfiles via manifest (works for `bazel run` and `bazel test`).
RUNFILES_MANIFEST="${RUNFILES_MANIFEST_FILE:-}"
if [ -z "${RUNFILES_MANIFEST}" ] || [ ! -f "${RUNFILES_MANIFEST}" ]; then
    for candidate in "${0}.runfiles_manifest" "${0}.runfiles/MANIFEST"; do
        if [ -f "${candidate}" ]; then
            RUNFILES_MANIFEST="${candidate}"
            break
        fi
    done
fi

if [ -f "${RUNFILES_MANIFEST}" ]; then
    rlocation() {
        # shellcheck disable=SC2016
        awk -v k="$1" '$1 == k { print $2; exit }' "${RUNFILES_MANIFEST}"
    }
elif [ -n "${RUNFILES_DIR:-}" ] && [ -d "${RUNFILES_DIR}" ]; then
    rlocation() {
        printf "%%s/%%s\\n" "${RUNFILES_DIR}" "$1"
    }
elif [ -d "${0}.runfiles" ]; then
    RUNFILES_DIR="${0}.runfiles"
    rlocation() {
        printf "%%s/%%s\\n" "${RUNFILES_DIR}" "$1"
    }
else
    rlocation() {
        echo ""
    }
fi

READELF="$(rlocation %s/%s/bin/llvm-readelf)"
if [ -z "${READELF}" ] || [ ! -x "${READELF}" ]; then
    READELF="$(rlocation %s/%s/bin/readelf)"
fi
if [ -z "${READELF}" ] || [ ! -x "${READELF}" ]; then
    READELF="readelf"
fi
export READELF
export MONGO_GDB_READELF="${READELF}"

OBJCOPY="$(rlocation %s/%s/bin/llvm-objcopy)"
if [ -z "${OBJCOPY}" ] || [ ! -x "${OBJCOPY}" ]; then
    OBJCOPY="$(rlocation %s/%s/bin/objcopy)"
fi
if [ -z "${OBJCOPY}" ] || [ ! -x "${OBJCOPY}" ]; then
    OBJCOPY="objcopy"
fi
export OBJCOPY

GDB="$(rlocation %s/%s/bin/gdb)"
if [ ! -x "${GDB}" ]; then
    # Best-effort fallback; the wrapper still execs a concrete gdb path below.
    GDB="gdb"
fi
export GDB

if [ -x "${GDB}" ]; then
    GDB_PREFIX="$(dirname "$(dirname "${GDB}")")"
    GDBHOME="$(dirname "${GDB_PREFIX}")/stow/gdb-$(basename "${GDB_PREFIX}")"
    export LD_LIBRARY_PATH="${GDB_PREFIX}/lib:${GDBHOME}/lib:${LD_LIBRARY_PATH:-}"
    GDB_ADD_INDEX="${GDB_PREFIX}/bin/gdb-add-index"
else
    GDB_ADD_INDEX="gdb-add-index"
fi
export GDB_ADD_INDEX
""" % (
        toolchain_repo,
        ctx.attr.version,
        toolchain_repo,
        ctx.attr.version,
        toolchain_repo,
        ctx.attr.version,
        toolchain_repo,
        ctx.attr.version,
        gdb_repo,
        ctx.attr.version,
    )

    ctx.file(
        "BUILD.bazel",
        """
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")

filegroup(
    name = "python_runtime",
    srcs = glob(["stow/python313-%s/**"]),
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

filegroup(
    name = "all_files",
    srcs = glob([
        "%s/**",
        "stow/gdb-%s/**",
        "stow/python313-%s/**",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "gdb_binary",
    srcs = ["%s/bin/gdb"],
    visibility = ["//visibility:public"],
)

sh_binary(
    name = "gdb",
    srcs = ["working_dir_gdb.sh"],
    data = [
        "%s/bin/gdb",
        ":gdb_runtime",
        ":python_runtime",
        "%s",
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
        "%s",
    ],
    visibility = ["//visibility:public"],
)

sh_binary(
    name = "gdb-add-index",
    srcs = ["working_dir_gdb_add_index.sh"],
    data = [
        "%s/bin/gdb",
        "%s/bin/gdb-add-index",
        ":gdb_runtime",
        ":python_runtime",
        "%s",
    ],
    visibility = ["//visibility:public"],
)

sh_binary(
    name = "gdb-generate-index",
    srcs = ["working_dir_gdb_generate_index.sh"],
    data = [
        "%s/bin/gdb",
        ":gdb_runtime",
        ":python_runtime",
        "%s",
    ],
    visibility = ["//visibility:public"],
)
""" % (
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.mongo_toolchain,
            python_env,
            ctx.attr.version,
            ctx.attr.mongo_toolchain,
            ctx.attr.version,
            ctx.attr.version,
            ctx.attr.mongo_toolchain,
            ctx.attr.version,
            ctx.attr.mongo_toolchain,
        ),
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
%s
# Keep GDB's derived symbol indexes in the user's local cache so repeated
# debugging sessions can reuse them. Respect an explicitly configured XDG
# cache directory and provide the same fallback used by the index actions when
# HOME is unavailable.
if [ -z "${XDG_CACHE_HOME:-}" ]; then
    if [ -n "${HOME:-}" ]; then
        export XDG_CACHE_HOME="${HOME}/.cache"
    else
        export XDG_CACHE_HOME="${RUNFILES_WORKING_DIRECTORY}/.cache"
    fi
fi
GDB_INDEX_CACHE_DIRECTORY="${XDG_CACHE_HOME}/gdb"
mkdir -p "${GDB_INDEX_CACHE_DIRECTORY}"

exec ${RUNFILES_WORKING_DIRECTORY}/../%s/%s/bin/gdb \\
    -iex "set index-cache directory ${GDB_INDEX_CACHE_DIRECTORY}" \\
    -iex "set index-cache enabled on" \\
    -iex "set auto-load safe-path %s/.gdbinit" \\
    "$@"
""" % (wrapper_gdb_setup, wrapper_binutils_setup, wrapper_python_setup, gdb_repo, ctx.attr.version, str(ctx.workspace_root)),
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
%s
${RUNFILES_WORKING_DIRECTORY}/external/%s/%s/bin/gdbserver localhost:1234 ${TEST_SRCDIR}/_main/${original_args[0]} "${@:2}"
""" % (wrapper_gdb_setup, wrapper_binutils_setup, wrapper_python_setup, gdb_repo, ctx.attr.version),
    )

    ctx.file(
        "working_dir_gdb_add_index.sh",
        """#!/bin/bash

set -euo pipefail

RUNFILES_WORKING_DIRECTORY="${BUILD_WORKING_DIRECTORY:-$(pwd)}"

cd "${RUNFILES_WORKING_DIRECTORY}"
%s
%s
%s
if [[ -z "${XDG_CACHE_HOME:-}" && -z "${HOME:-}" ]]; then
    export XDG_CACHE_HOME="${RUNFILES_WORKING_DIRECTORY}/.cache"
fi
if [ "$#" -eq 2 ]; then
    cp "$1" "$2"
    set -- "$2"
fi

if [ "$#" -ne 1 ]; then
    echo "Usage: gdb-add-index <binary>" >&2
    exit 1
fi

if [ ! -x "${GDB_ADD_INDEX}" ]; then
    echo "ERROR: could not locate the gdb-add-index executable in the GDB runfiles." >&2
    exit 1
fi

"${GDB_ADD_INDEX}" "$1"
""" % (wrapper_gdb_setup, wrapper_binutils_setup, wrapper_python_setup),
    )

    ctx.file(
        "working_dir_gdb_generate_index.sh",
        """#!/bin/bash

set -euo pipefail

RUNFILES_WORKING_DIRECTORY="${BUILD_WORKING_DIRECTORY:-$(pwd)}"

cd "${RUNFILES_WORKING_DIRECTORY}"
%s
%s
%s
if [[ -z "${XDG_CACHE_HOME:-}" && -z "${HOME:-}" ]]; then
    export XDG_CACHE_HOME="${RUNFILES_WORKING_DIRECTORY}/.cache"
fi

if [ "$#" -ne 2 ]; then
    echo "Usage: gdb-generate-index <binary> <index-bundle>" >&2
    exit 1
fi

# Split DWARF indexing spends significant time opening and reading DWO files.
# Use two worker threads per visible CPU to overlap I/O with index construction.
worker_threads="$((2 * $(nproc)))"

input_binary="$1"
index_bundle="$2"
temporary_directory="$(mktemp -d)"
trap 'rm -rf "${temporary_directory}"' EXIT

mkdir -p "${index_bundle}"
rm -f "${index_bundle}/gdb_index" "${index_bundle}/debug_names" "${index_bundle}/debug_str" "${index_bundle}/no_index"

"${GDB}" --batch -nx \\
    -iex 'set auto-load no' \\
    -iex 'set debuginfod enabled off' \\
    -iex "maint set worker-threads ${worker_threads}" \\
    -ex "file '${input_binary}'" \\
    -ex "save gdb-index -dwarf-5 '${temporary_directory}'"

input_basename="$(basename "${input_binary}")"
if [ -f "${temporary_directory}/${input_basename}.gdb-index" ]; then
    cp "${temporary_directory}/${input_basename}.gdb-index" "${index_bundle}/gdb_index"
elif [ -f "${temporary_directory}/${input_basename}.debug_names" ]; then
    cp "${temporary_directory}/${input_basename}.debug_names" "${index_bundle}/debug_names"
else
    touch "${index_bundle}/no_index"
fi

if [ -f "${temporary_directory}/${input_basename}.debug_str" ]; then
    cp "${temporary_directory}/${input_basename}.debug_str" "${index_bundle}/debug_str"
fi
""" % (wrapper_gdb_setup, wrapper_binutils_setup, wrapper_python_setup),
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
        mongo_toolchain = "@mongo_toolchain_v5//:all_files",
    )

def setup_gdb_toolchain_aliases(name = "setup_toolchains"):
    """Create unversioned and versioned gdb aliases.

    Args:
        name: Unused. Present to match other setup_*_aliases() signatures.
    """

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
        name = "gdb-add-index",
        actual = "@gdb_v5//:gdb-add-index",
    )
    native.alias(
        name = "gdb-generate-index",
        actual = "@gdb_v5//:gdb-generate-index",
    )
    native.alias(
        name = "gdb_toolchain_files",
        actual = "@gdb_v5//:all_files",
    )

    native.alias(
        name = "gdb_v5",
        actual = "@gdb_v5//:gdb",
    )
    native.alias(
        name = "gdbserver_v5",
        actual = "@gdb_v5//:gdbserver",
    )
    native.alias(
        name = "gdb-add-index_v5",
        actual = "@gdb_v5//:gdb-add-index",
    )
