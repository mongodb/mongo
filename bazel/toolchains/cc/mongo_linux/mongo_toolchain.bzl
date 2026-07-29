"""Repository rule for MongoDB's C/C++ toolchain."""

load("//bazel:utils.bzl", "generate_noop_toolchain", "get_toolchain_subs", "retry_download_and_extract")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain_version.bzl", "TOOLCHAIN_MAP")
load("//bazel/toolchains/cc/mongo_linux:mongo_mold.bzl", "MOLD_MAP")
load("//bazel/toolchains/cc/mongo_linux:sysroot_dump.bzl", "SYSROOT_ENV_VAR", "sysroot_dump_disabled_reason")

SKIP_TOOLCHAIN_ENVIRONMENT_VARIABLE = "no_c++_toolchain"

# BUILD file fragments substituted into mongo_toolchain.BUILD.tmpl at fetch
# time. The enable/disable decision is made here (in the repository rule, via
# sysroot_dump_disabled_reason) rather than at analysis time so that when the
# RBE sysroot is disabled — the default everywhere except opted-in Amazon
# Linux 2023 hosts — the generated BUILD file never references @rbe_sysroot
# and the toolchain's analysis graph is identical to what it was before
# sysroot support existed (no extra loads, selects, or config deps).
_SYSROOT_LOAD_ENABLED = """load("@rbe_sysroot//:sysroot_info.bzl", "SYSROOT_PATH")"""

_SYSROOT_DEFS_ENABLED = """# When the RBE sysroot is enabled, Bazel needs to know about both the original
# absolute include paths (used in -isystem flags) and their sysroot-resolved
# counterparts (where headers are actually found). Including both in
# cxx_builtin_include_directories lets Bazel validate either form.
SYSROOT_BUILTIN_INCLUDE_DIRECTORIES = (
    COMMON_BUILTIN_INCLUDE_DIRECTORIES +
    ["%sysroot%" + d for d in COMMON_BUILTIN_INCLUDE_DIRECTORIES if d.startswith("/")]
)

BUILTIN_SYSROOT = select({
    "@//bazel/config:use_rbe_sysroot_enabled": SYSROOT_PATH,
    "//conditions:default": "",
})

EFFECTIVE_BUILTIN_INCLUDE_DIRS = select({
    "@//bazel/config:use_rbe_sysroot_enabled": SYSROOT_BUILTIN_INCLUDE_DIRECTORIES,
    "//conditions:default": COMMON_BUILTIN_INCLUDE_DIRECTORIES,
})"""

_SYSROOT_DEFS_DISABLED = """# The RBE sysroot dump is disabled on this host (see sysroot_dump.bzl), so
# the toolchain uses the plain no-sysroot configuration.
BUILTIN_SYSROOT = ""

EFFECTIVE_BUILTIN_INCLUDE_DIRS = COMMON_BUILTIN_INCLUDE_DIRECTORIES"""

_SYSROOT_ALL_FILES_ENABLED = """ + select ({
        "@//bazel/config:use_rbe_sysroot_enabled": ["@rbe_sysroot//:sysroot_files"],
        "@//conditions:default": [],
    })"""

def _sysroot_substitutions(ctx):
    if sysroot_dump_disabled_reason(ctx) == None:
        return {
            "{sysroot_load}": _SYSROOT_LOAD_ENABLED,
            "{sysroot_defs}": _SYSROOT_DEFS_ENABLED,
            "{sysroot_all_files}": _SYSROOT_ALL_FILES_ENABLED,
        }
    return {
        "{sysroot_load}": "",
        "{sysroot_defs}": _SYSROOT_DEFS_DISABLED,
        "{sysroot_all_files}": "",
    }

def _toolchain_download(ctx):
    distro, arch, substitutions = get_toolchain_subs(ctx)
    substitutions.update(_sysroot_substitutions(ctx))

    skip_toolchain = ctx.os.environ.get(SKIP_TOOLCHAIN_ENVIRONMENT_VARIABLE, None)
    if skip_toolchain:
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("Skipping c++ toolchain download and defining noop toolchain due to {} being defined.".format(SKIP_TOOLCHAIN_ENVIRONMENT_VARIABLE))
        return None

    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = arch)

    if toolchain_key in TOOLCHAIN_MAP[ctx.attr.version]:
        toolchain_info = TOOLCHAIN_MAP[ctx.attr.version][toolchain_key]
        urls = toolchain_info["url"]
        sha = toolchain_info["sha"]

        ctx.report_progress("downloading {} mongo toolchain {}".format(toolchain_key, urls))
        retry_download_and_extract(
            ctx = ctx,
            tries = 5,
            url = urls,
            sha256 = sha,
        )

        if arch in MOLD_MAP:
            ctx.report_progress("Downloading mold from {}.".format(MOLD_MAP[arch]["url"]))
            retry_download_and_extract(
                ctx = ctx,
                tries = 5,
                url = MOLD_MAP[arch]["url"],
                sha256 = MOLD_MAP[arch]["sha"],
            )

        ctx.report_progress("generating toolchain " + ctx.attr.version + " build file")
        ctx.template(
            "BUILD.bazel",
            ctx.attr.build_tpl,
            substitutions = substitutions,
        )
        ctx.template(
            "mongo_toolchain_flags.bzl",
            ctx.attr.flags_tpl,
            substitutions = substitutions,
        )

    else:
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("Mongo toolchain " + ctx.attr.version + " not supported on this platform. Platform key not found: " + toolchain_key)

    return None

toolchain_download = repository_rule(
    implementation = _toolchain_download,
    environ = [
        SKIP_TOOLCHAIN_ENVIRONMENT_VARIABLE,
        # The generated BUILD file's sysroot fragments depend on whether the
        # RBE sysroot dump is enabled, which is keyed on this variable.
        SYSROOT_ENV_VAR,
    ],
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
            values = ["v5"],
            doc = "Mongodbtoolchain version.",
            mandatory = True,
        ),
        "flags_tpl": attr.label(
            doc = "Label denoting the toolchain flags template.",
            mandatory = True,
        ),
        "build_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_linux:mongo_toolchain.BUILD.tmpl",
            doc = "Label denoting the BUILD file template that gets installed in the repo.",
        ),
    },
)

def setup_mongo_toolchains(name = "setup_toolchains"):
    """Download/register the MongoDB C/C++ toolchain repositories.

    Args:
        name: Unused. Present to match public macro conventions.
    """
    toolchain_download(
        name = "mongo_toolchain_v5",
        version = "v5",
        flags_tpl = "//bazel/toolchains/cc/mongo_linux:mongo_toolchain_flags_v5.bzl",
    )

    native.register_toolchains(
        "@mongo_toolchain_v5//:mongo_toolchain",
    )

# Defines aliases for key targets inside the toolchain the user has chosen via
# //bazel/config:mongo_toolchain_version. For example, clang_tidy is a target inside the
# toolchains, so this function defines an alias called clang_tidy that points to
# @mongo_toolchain_vN//:clang_tidy, where N depends on the config value. This lets us use
# the name //:clang_tidy to refer to the clang_tidy of the configured toolchain.
def setup_mongo_toolchain_aliases(name = "setup_aliases"):
    """Create aliases to tools within the configured mongo toolchain.

    Args:
        name: Unused. Present to match public macro conventions.
    """

    # Map from target's name inside the toolchain to the name we want to alias it to.
    toolchain_targets = {
        "llvm_symbolizer": "llvm_symbolizer",
        "llvm_symbolizer_libs": "llvm_symbolizer_libs",
        "clang_format": "clang_format",
        "clang_tidy": "clang_tidy",
        "mongo_toolchain": "mongo_toolchain",
        "all_files": "toolchain_files",
    }
    selects = {}
    for target in toolchain_targets:
        selects[target] = {}

    for target in toolchain_targets:
        selects[target]["//bazel/config:mongo_toolchain_v5"] = "@mongo_toolchain_v5//:{}".format(target)

    for target, local_alias in toolchain_targets.items():
        native.alias(
            name = local_alias,
            actual = select(selects[target]),
        )

setup_mongo_toolchains_extension = module_extension(
    implementation = lambda ctx: setup_mongo_toolchains(),
)
