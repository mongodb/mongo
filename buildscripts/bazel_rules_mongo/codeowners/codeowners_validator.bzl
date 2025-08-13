"""Repository rules for codeowners validator download"""

load("//utils:downloads.bzl", "retry_download_and_extract")
load("//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "12615dc4b6051371abf95413efbc33a62aaa4aa5c23e2ff2fbdb8c682d51637d",
        "url": "https://github.com/mongodb-forks/codeowners-validator/releases/download/v0.1.5/codeowners-validator_Linux_arm64.tar.gz",
    },
    "linux_x86_64": {
        "sha": "40899685b2b24667710a7775fbdba773dc24c8dbbc44bced7a9ddcec2ffaf3c1",
        "url": "https://github.com/mongodb-forks/codeowners-validator/releases/download/v0.1.5/codeowners-validator_Linux_x86_64.tar.gz",
    },
    "macos_aarch64": {
        "sha": "02f82172bdeeb3819bded9544ff96440c57d9545253fe39e2eb603b6a17ec550",
        "url": "https://github.com/mongodb-forks/codeowners-validator/releases/download/v0.1.5/codeowners-validator_Darwin_arm64.tar.gz",
    },
    "macos_x86_64": {
        "sha": "9ea30001ff6e9748c5e2095c850a58a7f4db4ccd6ab6fd1718c701dfe029e94f",
        "url": "https://github.com/mongodb-forks/codeowners-validator/releases/download/v0.1.5/codeowners-validator_Darwin_x86_64.tar.gz",
    },
}

def _codeowners_validator_download(ctx):
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
    ctx.report_progress("downloading codeowners validator")
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
exports_files(["codeowners-validator"])
""",
    )

    return None

_codeowners_validator = repository_rule(
    implementation = _codeowners_validator_download,
    attrs = {},
)

def codeowners_validator():
    _codeowners_validator(name = "codeowners_validator")

codeowners_validator_extension = module_extension(
    implementation = lambda ctx: _codeowners_validator(name = "codeowners_validator"),
)
