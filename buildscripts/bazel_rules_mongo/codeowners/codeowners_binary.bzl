"""Repository rules for codeowners validator download"""

load("//utils:downloads.bzl", "retry_download_and_extract")
load("//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "877d4599c08684398e9d79c20402c9fb2220e4fd656d1904eb1bfebb2312e337",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.2/codeowners_1.2.2_linux_arm64.tar.gz",
    },
    "linux_x86_64": {
        "sha": "4256303d68afed6e07c29da1cf06d554d3b085fab23c86132f5aeeb8b223f2bb",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.2/codeowners_1.2.2_linux_amd64.tar.gz",
    },
    "macos_aarch64": {
        "sha": "70ccac2da525b12ed52b450a7f7fe7db0f013862f8b1b056ed7935bad454bf78",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.2/codeowners_1.2.2_darwin_arm64.tar.gz",
    },
    "macos_x86_64": {
        "sha": "10c700c18caf654817cff6d814195d315f1f53a31b4d4ecacffa0ad437fd2580",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.2/codeowners_1.2.2_darwin_amd64.tar.gz",
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

codeowners_binary_extension = module_extension(
    implementation = lambda ctx: _codeowners_binary(name = "codeowners_binary"),
)
