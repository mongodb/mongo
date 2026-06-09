load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")

_PERF_BINARIES = {
    "aarch64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/perf/441/perf-6.1.175-amazon2023-arm64.tgz",
        sha256 = "bba7d7029d302c0aa7855838b3ced006b229bd3f64d811d71d4de90f083e0f18",
    ),
    "x86_64": struct(
        url = "https://mdb-build-public.s3.amazonaws.com/perf/441/perf-6.1.175-amazon2023.tgz",
        sha256 = "2ca4adee7e467c43898a1a4b5154e7e549bb0da1e9709ce48015c13fd808cc6a",
    ),
}

# perf runs on the host during the (local) bolt training pipeline, so we
# download the archive matching the host architecture. The whole package is
# extracted (bin/perf plus its libexec/perf-core and lib64/traceevent helpers)
# so perf can locate its support files at runtime.
_BUILD_FILE_CONTENT = """
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "perf",
    srcs = ["bin/perf"],
)
"""

def _setup_perf_binaries_impl(repository_ctx):
    arch = ARCH_NORMALIZE_MAP.get(repository_ctx.os.arch)
    if arch not in _PERF_BINARIES:
        fail("perf binaries are not available for host architecture: " + repository_ctx.os.arch)

    binaries = _PERF_BINARIES[arch]
    repository_ctx.download_and_extract(
        # Implements retry by relisting the url multiple times to be used as a failover.
        url = [binaries.url] * 5,
        sha256 = binaries.sha256,
    )
    repository_ctx.file("BUILD.bazel", _BUILD_FILE_CONTENT)

setup_perf_binaries = repository_rule(
    implementation = _setup_perf_binaries_impl,
    attrs = {},
)
