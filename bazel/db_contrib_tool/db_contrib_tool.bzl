"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "e45c30a8c1e1adf47569661f59c2239ee7f4b0e9ddd298cf205fce7ecc9daefa",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "6cadfa717715c174c48e49baf7c305538e8b907b4b7364947caa303dec651124",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "71fef0159edf443cbce3be242c13f999a506cdf6962611df63d3dcc36e0137d9",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "26cc9f44d741e7cdce1b69bd12b9111c47e78a1cb8de02fe0a0cbd7263da963b",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "9f14cd09eee01ab82595dc6cc1819dbe96f6bb89e4661012d9e159231d999925",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "def80ad7ee23ccd25870b4576936b14155f5bea36f71a998d6ba59214c06b895",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "c01c17a22974c133c3f7c039684824773a3357cd7f669c95ded204cc52f4e7f7",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "accb4fe32b554e5ed5f3339b03cb4358138eea2d1f4e6e395f4ddaea2e282589",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.2.9/db-contrib-tool_v2.2.9_windows_x64.exe.gz",
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
