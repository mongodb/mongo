load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")

_BOLT_BINARIES = {
    "aarch64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/llvm-bolt/444/llvm-bolt-rhel88-arm64-ef752a078.tgz",
        sha256 = "8ff371db91902e632687de09061765f3c248714ba8b41415d4ff56c8e337f7f1",
    ),
    "x86_64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/llvm-bolt/444/llvm-bolt-rhel88-ef752a078.tgz",
        sha256 = "ba61e2f7fe7ea7d98c5c43b7d83c15b8df85137797bab157cfe8b84a7b369b1c",
    ),
}

# The bolt binaries run as build actions on the host (BOLT optimization runs
# locally, so host and exec architectures are the same), so we download the
# archive matching the host architecture.
_BUILD_FILE_CONTENT = """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "bolt",
    srcs = ["bin/llvm-bolt"],
)

filegroup(
    name = "perf2bolt",
    srcs = ["bin/perf2bolt"],
)

filegroup(
    name = "merge-fdata",
    srcs = ["bin/merge-fdata"],
)

filegroup(
    name = "libbolt_rt_instr",
    srcs = ["lib/libbolt_rt_instr.a"],
)
"""

def _setup_bolt_binaries_impl(repository_ctx):
    arch = ARCH_NORMALIZE_MAP.get(repository_ctx.os.arch)
    if arch not in _BOLT_BINARIES:
        fail("BOLT binaries are not available for host architecture: " + repository_ctx.os.arch)

    binaries = _BOLT_BINARIES[arch]
    repository_ctx.download_and_extract(
        # Implements retry by relisting the url multiple times to be used as a failover.
        url = [binaries.url] * 5,
        sha256 = binaries.sha256,
    )
    repository_ctx.file("BUILD.bazel", _BUILD_FILE_CONTENT)

setup_bolt_binaries = repository_rule(
    implementation = _setup_bolt_binaries_impl,
    attrs = {},
)
