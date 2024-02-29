load("//bazel:utils.bzl", "retry_download_and_extract")

_OS_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}

_ARCH_MAP = {
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
    "ppc64le": "@platforms//cpu:ppc64le",
}

_ARCH_NORMALIZE_MAP = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "ppc64le": "ppc64le",
}

URLS_MAP = {
    "linux_x86_64": {
        "sha": "e2bf59dacb789bd3ed708bafb7bf4e432f611f19d6b888340e3b73eee6949b31",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_amazon2_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "linux_aarch64": {
        "sha": "269e54f97d9049d24d934f549a8963c15c954a5cb6fc0d75bbbcfb78df3c3647",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_amazon2_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "linux_ppc64le": {
        "sha": "e9ac010977f6b92d301174a5749c06e4678a0071556745ea3681a2825b6f7bd1",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel81-ppc64le/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel81_ppc64le_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
}

def _toolchain_download(ctx):
    if ctx.attr.os:
        os = ctx.attr.os
    else:
        os = "linux"

    if ctx.attr.arch:
        arch = ctx.attr.arch
    else:
        arch = ctx.os.arch

    arch = _ARCH_NORMALIZE_MAP[arch]

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

    os_arch = "{os}_{arch}".format(os = os, arch = arch)

    if os_arch in URLS_MAP:
        platform_info = URLS_MAP[os_arch]
        urls = platform_info["url"]
        sha = platform_info["sha"]

        ctx.report_progress("downloading mongo toolchain")
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
            values = ["amd64", "aarch64", "amd64", "x86_64", "ppc64le"],
            doc = "Host architecture.",
        ),
        "build_tpl": attr.label(
            default = "//bazel/toolchains:mongo_toolchain.BUILD",
            doc = "Label denoting the BUILD file template that get's installed in the repo.",
        ),
    },
)
