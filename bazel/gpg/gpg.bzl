"""Repository rules for gpg bundle download"""

load("//bazel:utils.bzl", "retry_download_and_extract")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "d7197d8b8ad4dc4ef6c27eb03c6cc565e00f994b33011da89f37dacc92810228",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/gpg_bundle-aarch64.tar.gz",
    },
    "linux_x86_64": {
        "sha": "66608c5dcfd4580ec7e7dfcf8dd16df73b563674222bf3b9785d853b3d2052ee",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/gpg_bundle-x86_64.tar.gz",
    },
    "linux_s390x": {
        "sha": "1fff70fce14abfa83b08df7465929c0b98e5c12c7bff001c4fbd82adaf82c8bd",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/gpg_bundle-s390x.tar.gz",
    },
    "linux_ppc64le": {
        "sha": "3f2ecdfb99c49d148f92973e5164821de663984e5a02cd6a1686ce32f1c1d9f9",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/gpg-binaries/SERVER-115285/gpg_bundle-ppc64le.tar.gz",
    },
}

def _gpg_bundle_repo_impl(ctx):
    os = ctx.os.name
    os_norm = OS_NORMALIZE_MAP.get(ctx.os.name)
    if os_norm != "linux":
        ctx.file(
            "BUILD.bazel",
            content = """package(default_visibility = ["//visibility:public"])
filegroup(name = "gpg_bins", srcs = glob([]))
filegroup(name = "gpg_libs", srcs = glob([]))
""",
        )
        return

    arch = ctx.os.arch
    os_constraint = OS_NORMALIZE_MAP[os]
    arch_constraint = ARCH_NORMALIZE_MAP[arch]
    platform_key = "{os}_{arch}".format(os = os_constraint, arch = arch_constraint)

    if platform_key not in URLS_MAP:
        fail("Unsupported platform for gpg bundle: {k}. Supported: {supported}".format(
            k = platform_key,
            supported = ", ".join(sorted(URLS_MAP.keys())),
        ))

    platform_info = URLS_MAP[platform_key]
    ctx.report_progress("downloading gpg bundle")
    retry_download_and_extract(
        ctx = ctx,
        tries = 5,
        url = platform_info["url"],
        sha256 = platform_info["sha"],
    )

    # BUILD file: include all bin/* and libs/** in runfiles
    ctx.file(
        "BUILD.bazel",
        content = """package(default_visibility = ["//visibility:public"])
filegroup(
    name = "gpg_libs",
    srcs = glob(["gpg_bundle-*/libs/**"]),)
    
filegroup(
    name = "gpg_bins",
    srcs = glob(["gpg_bundle-*/bin/*"]),)

""",
    )

_gpg_bundle_repo = repository_rule(
    implementation = _gpg_bundle_repo_impl,
    attrs = {},
)

def gpg():
    _gpg_bundle_repo(name = "gpg")
