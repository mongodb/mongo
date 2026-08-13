load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")

_BOLT_BINARIES = {
    "aarch64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/llvm-bolt/452/llvm-bolt-rhel88-arm64-ef752a078.tgz",
        sha256 = "634cdade79e1954fcc81672f44484fbb127156d6bcaafc5a3a6b739e9691467b",
    ),
    "x86_64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/llvm-bolt/452/llvm-bolt-rhel88-ef752a078.tgz",
        sha256 = "d13d7053ccd089b8fa4852434d21f6e85c6314ccd73e0a927d6bed6d3fecb8f4",
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
