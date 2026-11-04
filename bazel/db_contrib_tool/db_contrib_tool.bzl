"""Repository rules for db-contrib-tool"""

load("//bazel:utils.bzl", "retry_download")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP", "OS_NORMALIZE_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "2aa29e982b330c715299b727517490d4a5527f6c59fdc5ad68c1928a1135297d",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_linux_arm64.gz",
    },
    "linux_x86_64": {
        "sha": "8030a00ba1be2b6ea03fd7705285f628f70e6a1489e73d4db3cc90abc9768c9b",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_linux_x64.gz",
    },
    "linux_s390x": {
        "sha": "e1ca99fad58b32ba6936690657a669f8dbf84de1802797b1e2d1bfd472048f60",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_linux_s390x.gz",
    },
    "rhel8_ppc64le": {
        "sha": "4f7f8313bb833346ac9b3fcc39729df5a92793b6a873bf5c40693d96bee50519",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_rhel8_ppc64le.gz",
    },
    "rhel9_ppc64le": {
        "sha": "c777904180f2869e7f1413dfc1b888601aac00f022521238daa235221d891e04",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_rhel9_ppc64le.gz",
    },
    "macos_x86_64": {
        "sha": "6fc6e83bc7986a87dc099f879a6b322210f876c3610004b60df59fea33bb2763",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_darwin_x64.gz",
    },
    "macos_aarch64": {
        "sha": "30ef52a9605c939ddefea7d9094f935e991944ae697181365ead775bb8ea6d4f",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_darwin_arm64.gz",
    },
    "windows_x86_64": {
        "sha": "c99a1b09c816da59cb7c0783a31c017bfa265ea1e8a4dd6247744fd6e00b6ec7",
        "url": "https://mdb-build-public.s3.amazonaws.com/db-contrib-tool-binaries/v2.5.1/db-contrib-tool_v2.5.1_windows_x64.exe.gz",
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
