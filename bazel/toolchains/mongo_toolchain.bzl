load("//bazel:utils.bzl", "get_host_distro_major_version", "retry_download_and_extract")
load("//bazel/toolchains:mongo_toolchain_version.bzl", "TOOLCHAIN_MAP")

_OS_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}

_ARCH_MAP = {
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
    "ppc64le": "@platforms//cpu:ppc64le",
    "s390x": "@platforms//cpu:s390x",
}

_ARCH_NORMALIZE_MAP = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "ppc64le": "ppc64le",
    "s390x": "s390x",
}

def _toolchain_download(ctx):
    if ctx.attr.os:
        os = ctx.attr.os
    else:
        os = ctx.os.name

    if ctx.attr.arch:
        arch = ctx.attr.arch
    else:
        arch = ctx.os.arch

    arch = _ARCH_NORMALIZE_MAP[arch]

    if os != "linux":
        # BUILD file is required for a no-op
        substitutions = {
            "{platforms_arch}": "arm64",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
        }
        ctx.template(
            "BUILD.bazel",
            ctx.attr.build_tpl,
            substitutions = substitutions,
        )
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)
        return None

    if arch == "aarch64":
        substitutions = {
            "{platforms_arch}": "arm64",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
        }
    elif arch == "x86_64":
        substitutions = {
            "{platforms_arch}": "x86_64",
            "{bazel_toolchain_cpu}": "k8",
            "{arch}": arch,
        }
    elif arch == "ppc64le":
        substitutions = {
            "{platforms_arch}": "ppc64le",
            "{bazel_toolchain_cpu}": "ppc",
            "{arch}": arch,
        }
    elif arch == "s390x":
        substitutions = {
            "{platforms_arch}": "s390x",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
        }

    distro = get_host_distro_major_version(ctx)
    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = arch)

    if toolchain_key in TOOLCHAIN_MAP:
        toolchain_info = TOOLCHAIN_MAP[toolchain_key]
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

        ctx.report_progress("generating toolchain build file")

        ctx.template(
            "BUILD.bazel",
            ctx.attr.build_tpl,
            substitutions = substitutions,
        )
    else:
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)

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
        "build_tpl": attr.label(
            default = "//bazel/toolchains:mongo_toolchain.BUILD",
            doc = "Label denoting the BUILD file template that get's installed in the repo.",
        ),
    },
)
