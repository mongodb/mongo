"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "c17e01efd76490d26a687d8198eba30992ed997742ec1d3f6f80444f0bb3cfb3",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "9a3345195569ba106e699297bd5c692c68c83c4711f7d44ba1293a707ab66a3d",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "5eeb99d829ced8586c2d68df8b5b5b2e986a06838f2d589d32fdf4c0237a4568",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "3abc47feed9ae80f113a6e740a94e235f894efedcd7c4b38ad2bde354fbcd395",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "79121a0798ae644d5d9417405d1d790ea73e3c83e91ef6ed96ec43b03bcdbd2c",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "cd2cd3db5c2944244267467f8014f648d73729cdbaa1b5a30d7ee60bbeabd48f",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "b482d4769dfae740c79ec9a2e1e036272ab3985a0b0c400a750d5d939aae41bd",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "89de6772413f1e6665bfd9a7f30aeecf8bfacc9f1af8eccde687470d09cde640",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.4.6/db-contrib-tool_v2.4.6_windows_x64.exe.gz",
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
