"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "c083405331439d76e1dcae8bbb3e90c6039fc7333e0160f46dff251dd506a637",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "937985fbf56a3312787e15ade4cafc208f2b81ee483b096aead34c0a1816b627",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "8e06a8386ace14875d70d0f5e8de13d982902f76c1eff70250dbb49e0c56703f",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "c0a6616aa07c9fdb2e3b867833718b7c973bfee345d0e781b943430af10784d3",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "f541856ecc0698b7dc1a96b84d8af326f69c6b72997d6d6dd98ee713e88343c0",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "4e1e8d0c3969668037fd2a2fd550ed6e42b85a1fbbed5cc8f891900c664cb138",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "045e1b041a4ab1fc7d0cd45ef50f6d5e3403e6597412b1509491a6865dd5296e",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "61a53f57845d5af7a95e50144154c04c116c96b118cb19fcc3c02f474b96204c",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.0/db-contrib-tool_v2.4.0_windows_x64.exe.gz",
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

_db_contrib_tool = repository_rule(
    implementation = _db_contrib_tool_download,
    attrs = {},
)

def db_contrib_tool():
    _db_contrib_tool(name = "db_contrib_tool")
