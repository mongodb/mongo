"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "852e399493231ead35ea4d714146af8cb0f69b54363c56ec75e4dc22c11bdcca",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "af67b230381fe3ba7e79789641fe695db194cbdea50bbe1e40ab005967e2f05c",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "a5bb4f5830605f5d43f8c0d430158cacf410d392d4a3f68d9db90431eb07069e",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "fe0814827c55e20c4605ee781580c948f2436e78f243b4660b58684fc429ac8b",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "0a4b59595a015baa6f1d5575f16550f4f3c43b04b16eb1a007c098815b7d5f43",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "0498d20b0f23d4667962722cf9d10813bec6253805565f3cc19d4a4e2fe27270",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "0f568ed1717b2e9b5b163e8d1f128fa9c7b25c1d4726282c1a1e0a561b03a16f",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "cc9c0a5b9e023903104f72bc86ded14ad4efe0be01e0fd03f9b3a25084ed7ad5",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.5/db-contrib-tool_v2.4.5_windows_x64.exe.gz",
    },
}

def _get_python(ctx):
    os_constraint = OS_NORMALIZE_MAP[ctx.os.name]
    if os_constraint == "windows":
        return ctx.path(Label("@py_host//:dist/python.exe"))
    return ctx.path(Label("@py_host//:dist/bin/python3"))

def _extract_gz_executable(ctx, src, dst):
    """Extract a gzip-compressed file using the toolchain Python's gzip module, and mark the output as executable."""
    python = _get_python(ctx)
    result = ctx.execute([
        python,
        "-c",
        "import gzip,shutil,sys,os; shutil.copyfileobj(gzip.open(sys.argv[1],'rb'),open(sys.argv[2],'wb')); os.chmod(sys.argv[2], 0o755)",
        src,
        dst,
    ])
    if result.return_code != 0:
        fail("Failed to extract {}: {}".format(src, result.stderr))

def _detect_rhel_major(ctx):
    """Detect RHEL major version from the kernel release string (e.g. el8, el9)."""
    result = ctx.execute(["uname", "-r"])
    if result.return_code != 0:
        fail("db_contrib_tool: failed to detect RHEL major version: `uname -r` exited with {}: {}".format(result.return_code, result.stderr))
    for part in result.stdout.strip().replace("-", ".").split("."):
        if part.startswith("el") and part[2:].isdigit():
            return str(min(int(part[2:]), 9))
    fail("db_contrib_tool: failed to detect RHEL major version from kernel release: {}".format(result.stdout.strip()))

def _db_contrib_tool_download(ctx):
    os = ctx.os.name
    arch = ctx.os.arch
    os_constraint = OS_NORMALIZE_MAP[os]
    arch_constraint = ARCH_NORMALIZE_MAP[arch]
    if arch_constraint == "ppc64le":
        platform_key = "rhel{}_ppc64le".format(_detect_rhel_major(ctx))
    else:
        platform_key = "{os}_{arch}".format(os = os_constraint, arch = arch_constraint)
    if platform_key not in URLS_MAP:
        fail("db_contrib_tool: unsupported platform: " + platform_key)
    platform_info = URLS_MAP[platform_key]
    ctx.report_progress("downloading db-contrib-tool")
    retry_download(
        ctx = ctx,
        output = "db-contrib-tool.gz",
        tries = 3,
        url = platform_info["url"],
        sha256 = platform_info["sha"],
    )

    _extract_gz_executable(ctx, "db-contrib-tool.gz", "db-contrib-tool-bin")

    ctx.file(
        "BUILD.bazel",
        """
# Visibility restricted: db-contrib-tool downloads binaries from external sources.
# Non-hermetic external downloads should not influence the core build graph.
package(default_visibility = [
    "@//:__pkg__",
    "@//bazel/db_contrib_tool:__pkg__",
    "@//bazel/resmoke/multiversion:__pkg__",
    "@//bazel/resmoke/mongot:__pkg__",
])
load("@bazel_skylib//rules:native_binary.bzl", "native_binary")

native_binary(
    name = "db-contrib-tool",
    src = "db-contrib-tool-bin",
    out = "db-contrib-tool",
)
""",
    )

    return None

db_contrib_tool = repository_rule(
    implementation = _db_contrib_tool_download,
    attrs = {},
)
