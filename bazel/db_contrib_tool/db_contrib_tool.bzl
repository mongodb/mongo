"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "ce91741870084c3505010f07ad407e9eac24ab569bd0e7cbd185d3dfae302434",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "082901af33d94c1ab0cc60efd84210840ed648817fd0da6bdac59e57d2723b95",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "c8edf39f66dd86003754294b218ed9358ed5510da6cd42146591fc40afbd6884",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "42089898d6740ed5d200a9d498b97a0f26afb5028651ae0ab9a60c552ec5fbc3",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "0ff1f4fcd177eae63ccb0c3a61a64a879204113a81e73360b265f12ff3de5e3d",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "3f7126ce8828d4c87796336937f495a0c0a314f2c051119a9d3e78cc82786aa9",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "a2b1cbffe7b1221497f38e1e1ad1cc32b2a7f3d1a1ae0d43421b89f09d93385c",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "b379e2c9d332826d2c0bc04ed83b2b2609a2a02f18c593ff80e46c9655258271",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.4/db-contrib-tool_v2.4.4_windows_x64.exe.gz",
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
