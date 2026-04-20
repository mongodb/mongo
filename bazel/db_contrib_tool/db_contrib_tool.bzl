"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "000189ea41fc498a9090d39cb1d0fbf426dfe6ba119dcd60cab802bcb261bd4d",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "c7dd52b4dc706f6ee6a2f553271f4f57a6013cf3914363802427aaf38732d2ec",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "de5e149c041b4f982b72579e499f47ac16bac6c7df6c5326d7d225e00c8a5a40",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "0f6a380bd881d2423195d1338389d4ed305b6ad2fff6e773776f40896e4d58a8",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "92ae51c9ee0b343fc6723e0f6b9d529a3f93e1f4f54042193b77c762b8911c4e",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "42dcc92c2914214783ddec659a157dcef0aadc1a03bd29730c69511d8ad84912",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_darwin_x64.gz",
    },
    "windows_x86_64": {
        "sha": "da881cf80ab10ae98ade5fd7ea43337b26b5f674fabdbd11c5d1644804a8f089",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.3/db-contrib-tool_v2.2.3_windows_x64.exe.gz",
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
])
load("@bazel_skylib//rules:native_binary.bzl", "native_binary")

native_binary(
    name = "db-contrib-tool",
    src = "db-contrib-tool-bin",
    data = [
        
    ],
    out = "db-contrib-tool",
    env = {}
)
""",
    )

    return None

_db_contrib_tool = repository_rule(
    implementation = _db_contrib_tool_download,
    attrs = {},
)

def db_contrib_tool():
    _db_contrib_tool(name = "db_contrib_tool")
