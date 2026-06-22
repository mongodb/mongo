"""Repository rules for codeowners validator download"""

load("//utils:downloads.bzl", "retry_download_and_extract")
load("//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "9b4b4ba76182442201527d72d5a74cc47585b8186ccd7e9d3907577c15594edc",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.3/codeowners_1.2.3_linux_arm64.tar.gz",
    },
    "linux_x86_64": {
        "sha": "8e4456feb381013c447a887737bd0d97ea8b9691440466f1ac58486ea961f25b",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.3/codeowners_1.2.3_linux_amd64.tar.gz",
    },
    "macos_aarch64": {
        "sha": "5b8b240626f0cd03fa93b5294fc3a5998df8bf17786e87e0b5ca244f7c718e39",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.3/codeowners_1.2.3_darwin_arm64.tar.gz",
    },
    "macos_x86_64": {
        "sha": "fc8665f2d910de9d1d2ee46ac95df590070a426dbd8cbe15d0254691d8f3f37c",
        "url": "https://github.com/mongodb-forks/codeowners/releases/download/v1.2.3/codeowners_1.2.3_darwin_amd64.tar.gz",
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
