"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "09b8dcd02a566ad941ba631b3a32a0b849d06a4de11752fbf4869158a655e9ab",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "97ffde68d9182ac5fe605b9adbf51ad6262a5fef9d307d6f28bbb975a41677c2",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "4bf2a2b359a060d3c19dba89327d47168c88ee271db06cb39374a368fffcac84",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "73122d4858ad6b4aac48541e387e01c1e0ea2d0a50aee6efe2b4873389fb4576",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "4b3c6fbfe2323d4531ec34db524dddc910e746d2bcb9b7f7f78ac777e60a0c76",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "c900b9561b0a665fe9745989b1c106696c51942f200efd275339522786f8cc9e",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "8883b6d69c885f0d5deda2a81aff9629afa9c704edbff196fc79a7f33f1cc177",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "6f9098bc6a6ad2bea9accf985d7daacc85c58fdfd0f2c86c063ea2cd80db3666",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.6.0/db-contrib-tool_v2.6.0_windows_x64.exe.gz",
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
