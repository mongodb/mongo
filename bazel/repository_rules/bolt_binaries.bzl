load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")

_BOLT_BINARIES = {
    "aarch64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/llvm-bolt/441/llvm-bolt-rhel88-arm64-ee20b106e.tgz",
        sha256 = "973071c5bec7f64794cc5a9935f1bed80c4f0895648de55b37d49c43fcd5ed71",
    ),
    "x86_64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/llvm-bolt/441/llvm-bolt-rhel88-ee20b106e.tgz",
        sha256 = "b1ec25d2a479a990668d3992bd0cba9cc94d7f36213932fd56ed987009b8591d",
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
