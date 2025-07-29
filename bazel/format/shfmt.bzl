"""Repository rules for shfmt binary download"""

load("//bazel:utils.bzl", "retry_download")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "9d23013d56640e228732fd2a04a9ede0ab46bc2d764bf22a4a35fb1b14d707a8",
        "url": "https://github.com/mvdan/sh/releases/download/v3.10.0/shfmt_v3.10.0_linux_arm64",
    },
    "linux_x86_64": {
        "sha": "1f57a384d59542f8fac5f503da1f3ea44242f46dff969569e80b524d64b71dbc",
        "url": "https://github.com/mvdan/sh/releases/download/v3.10.0/shfmt_v3.10.0_linux_amd64",
    },
    "macos_aarch64": {
        "sha": "86030533a823c0a7cd92dee0f74094e5b901c3277b43def6337d5e19e56fe553",
        "url": "https://github.com/mvdan/sh/releases/download/v3.10.0/shfmt_v3.10.0_darwin_arm64",
    },
    "macos_x86_64": {
        "sha": "ef8d970b3f695a7e8e7d40730eedd2d935ab9599f78a365f319c515bc59d4c83",
        "url": "https://github.com/mvdan/sh/releases/download/v3.10.0/shfmt_v3.10.0_darwin_amd64",
    },
    "windows_x86_64": {
        "sha": "6e4c6acd38de7b4b1ba8f8082b9e688df8c9b861d3f8b2e7bb1b7270201a3587",
        "url": "https://github.com/mvdan/sh/releases/download/v3.10.0/shfmt_v3.10.0_windows_amd64.exe",
    },
}

def _shfmt_download(ctx):
    """
    Downloads a shfmt binary

    Args:
        ctx: Repository context.
    """
    os = ctx.os.name
    arch = ctx.os.arch
    os_constraint = OS_NORMALIZE_MAP[os]
    arch_constraint = ARCH_NORMALIZE_MAP[arch]
    platform_info = URLS_MAP["{os}_{arch}".format(os = os_constraint, arch = arch_constraint)]
    ctx.report_progress("downloading shfmt")
    retry_download(
        ctx = ctx,
        executable = True,
        output = "shfmt",
        tries = 5,
        url = platform_info["url"],
        sha256 = platform_info["sha"],
    )
    ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])
exports_files(["shfmt"])
""",
    )

    return None

_shfmt = repository_rule(
    implementation = _shfmt_download,
    attrs = {},
)

def shfmt():
    _shfmt(name = "shfmt")
