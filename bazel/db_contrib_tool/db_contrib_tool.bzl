"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "75b9816f262e828e45fd336f067c0721e38d75c7d6d40f817648822c80ff3b1e",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "3de6cce6ef60998dbb09ff30a0670d1ac5499067269aa5c2f403ea44ea4ca4db",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "49ca8380859d03be63457d031f8db7054f3f88f6e2b47da66748ded4333ed756",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "bb11d8cef10d89975a1cc31b9e07c1ec9a1d1b4c98367362f458568ea25b2995",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "8ee1febb2a22034b66fc3acebdd3eee3fc1e51b58ac532852283a6b719a1b9ec",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "53aea9e93ecc8865b0dbb95432d56879737b58187b56e22835403c3c883c438c",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "81495a8f1be9d60fa20d005e0f4e349f51a610e1c750c532db8640eeab4a3c48",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "47e0946c0703043968ef758f3b1cccfa4d5ef690763ededb89645b0765a3a4b8",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.11/db-contrib-tool_v2.2.11_windows_x64.exe.gz",
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
