"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("@bazel_rules_mongo//utils:platforms_normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "906ad7ef82ebb213e00a7e24d3d58aee7f727f1f2bb5c231273ba0403e12d48f",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "aee84712a1c6de2ed6e3f1b8411ba00deb64ab28f1fca7d121f4d15c65ec4c43",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "3c439b6f73526c35710da794cfc3fccbbac927c597565c038b64890fd3d6e5d4",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "6a1c750daf174dfd9afffb94293bba8835913702151ecfac81b85f594843c6a6",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "69bc9d2e860d83789e39b7772826b7b01b56ba88799adb7d836eb22bf1ad59e8",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "080eff2e0ccafd4a22c3f974ef8de413daff44245ec3d18e3256208f94730d53",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "6023c10aba3726f6376f7fdfcba72fb441fbaec5a6d3e2202d4bd0a83dbf1278",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "59bbc1c1fbb3287698d1299b1f90501655dff974004ae12f668558bb521b3256",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.1/db-contrib-tool_v2.4.1_windows_x64.exe.gz",
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

_db_contrib_tool = repository_rule(
    implementation = _db_contrib_tool_download,
    attrs = {},
)

def db_contrib_tool():
    _db_contrib_tool(name = "db_contrib_tool")
