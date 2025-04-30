"""Repository rules for codeowners validator download"""

load("//utils:downloads.bzl", "retry_download_and_extract")
load("//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "bb3a283e2bd6c50d8b383c5a8b99179ded65eefdbd95945826a61f860ce531f4",
        "url": "https://github.com/hmarr/codeowners/releases/download/v1.2.1/codeowners_1.2.1_linux_arm64.tar.gz",
    },
    "linux_x86_64": {
        "sha": "94f9f9ec43dba151816b5c2fd98698afbfd03d5ac63db77d2d8c2cf77b326bb0",
        "url": "https://github.com/hmarr/codeowners/releases/download/v1.2.1/codeowners_1.2.1_linux_amd64.tar.gz",
    },
    "macos_aarch64": {
        "sha": "1a271d2a3960491d7fceffdca741e7a3830cb2ab5013723ed8f9efe04dd3d9c1",
        "url": "https://github.com/hmarr/codeowners/releases/download/v1.2.1/codeowners_1.2.1_darwin_arm64.tar.gz",
    },
    "macos_x86_64": {
        "sha": "39d5868f50a3716af61c1bd4722b9f840f07a005d3018b20483de26b10ced19a",
        "url": "https://github.com/hmarr/codeowners/releases/download/v1.2.1/codeowners_1.2.1_darwin_amd64.tar.gz",
    },
}

def _codeowners_binary_download(ctx):
    """
    Downloads a codeowners validator binary

    Args:
        ctx: Repository context.
    """
    os = ctx.os.name
    arch = ctx.os.arch
    os_constraint = OS_NORMALIZE_MAP[os]
    arch_constraint = ARCH_NORMALIZE_MAP[arch]
    platform_info = URLS_MAP["{os}_{arch}".format(os = os_constraint, arch = arch_constraint)]
    ctx.report_progress("downloading codeowners binary")
    retry_download_and_extract(
        ctx = ctx,
        tries = 5,
        url = platform_info["url"],
        sha256 = platform_info["sha"],
    )

    ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])
exports_files(["codeowners"])
""",
    )

    return None

_codeowners_binary = repository_rule(
    implementation = _codeowners_binary_download,
    attrs = {},
)

def codeowners_binary():
    _codeowners_binary(name = "codeowners_binary")
