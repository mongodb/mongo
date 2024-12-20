load("//bazel:utils.bzl", "get_host_distro_major_version", "retry_download_and_extract")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")
load("//bazel/toolchains:mongo_toolchain_version.bzl", "TOOLCHAIN_MAP")

def _generate_noop_toolchain(ctx, substitutions):
    # BUILD file is required for a no-op
    ctx.file(
        "BUILD.bazel",
        "# {} toolchain not supported on this platform".format(ctx.attr.version),
    )
    ctx.template(
        "mongo_toolchain_flags.bzl",
        ctx.attr.flags_tpl,
        substitutions = substitutions,
    )

def _toolchain_download(ctx):
    if ctx.attr.os:
        os = ctx.attr.os
    else:
        os = ctx.os.name

    if ctx.attr.arch:
        arch = ctx.attr.arch
    else:
        arch = ctx.os.arch

    arch = ARCH_NORMALIZE_MAP[arch]

    version = ctx.attr.version

    if os != "linux":
        substitutions = {
            "{platforms_arch}": "arm64",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
        }
        _generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)
        return None

    if arch == "aarch64":
        substitutions = {
            "{platforms_arch}": "arm64",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
        }
    elif arch == "x86_64":
        substitutions = {
            "{platforms_arch}": "x86_64",
            "{bazel_toolchain_cpu}": "x86_64",
            "{arch}": arch,
            "{version}": version,
        }
    elif arch == "ppc64le":
        substitutions = {
            "{platforms_arch}": "ppc64le",
            "{bazel_toolchain_cpu}": "ppc",
            "{arch}": arch,
            "{version}": version,
        }
    elif arch == "s390x":
        substitutions = {
            "{platforms_arch}": "s390x",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
        }

    distro = get_host_distro_major_version(ctx)
    if distro == None:
        fail("Failed to get mongo toolchain supported distribution for os {}".format(os))

    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = arch)

    if toolchain_key in TOOLCHAIN_MAP[version]:
        toolchain_info = TOOLCHAIN_MAP[version][toolchain_key]
        urls = toolchain_info["url"]
        sha = toolchain_info["sha"]

        ctx.report_progress("downloading {} mongo toolchain {}".format(toolchain_key, urls))
        print("downloading {} mongo toolchain {}".format(toolchain_key, urls))
        retry_download_and_extract(
            ctx = ctx,
            tries = 5,
            url = urls,
            sha256 = sha,
        )

        ctx.report_progress("generating toolchain " + version + " build file")
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
        _generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("Mongo toolchain " + version + " not supported on " + distro + " and " + arch + ". Toolchain key not found: " + toolchain_key)

    return None

toolchain_download = repository_rule(
    implementation = _toolchain_download,
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
        "flags_tpl": attr.label(
            doc = "Label denoting the toolchain flags template.",
            mandatory = True,
        ),
        "build_tpl": attr.label(
            default = "//bazel/toolchains:mongo_toolchain.BUILD",
            doc = "Label denoting the BUILD file template that gets installed in the repo.",
        ),
    },
)

def setup_mongo_toolchains():
    toolchain_download(
        name = "mongo_toolchain_v4",
        version = "v4",
        flags_tpl = "//bazel/toolchains:mongo_toolchain_flags_v4.bzl",
    )

    toolchain_download(
        name = "mongo_toolchain_v5",
        version = "v5",
        flags_tpl = "//bazel/toolchains:mongo_toolchain_flags_v5.bzl",
    )

    native.register_toolchains(
        "@mongo_toolchain_v4//:all",
        "@mongo_toolchain_v5//:all",
    )

# Defines aliases for key targets inside the toolchain the user has chosen via
# //bazel/config:mongo_toolchain_version. For example, clang_tidy is a target inside the
# toolchains, so this function defines an alias called clang_tidy that points to
# @mongo_toolchain_vN//:clang_tidy, where N depends on the config value. This lets us use
# the name //:clang_tidy to refer to the clang_tidy of the configured toolchain.
def setup_mongo_toolchain_aliases():
    # Map from target's name inside the toolchain to the name we want to alias it to.
    toolchain_targets = {
        "clang_tidy": "clang_tidy",
        "mongo_toolchain": "mongo_toolchain",
        "all_files": "toolchain_files",
    }
    selects = {}
    for target in toolchain_targets:
        selects[target] = {}

    for version in ("v4", "v5"):
        toolchain_name = "mongo_toolchain_{}".format(version)
        option_name = "//bazel/config:mongo_toolchain_{}".format(version)
        for target in toolchain_targets:
            selects[target][option_name] = "@{}//:{}".format(toolchain_name, target)

    for target, local_alias in toolchain_targets.items():
        native.alias(
            name = local_alias,
            actual = select(selects[target]),
        )
