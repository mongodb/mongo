load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _setup_wasi_deps(rctx):
    if (rctx.os.arch == "aarch64"):
        rctx.download_and_extract(
            "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-arm64-linux.tar.gz",
            output = "",
            sha256 = "6f2977942308d91b0123978da3c6a0d6fce780994b3b020008c617e26764ea40",
            stripPrefix = "wasi-sdk-30.0-arm64-linux",
        )
    elif (rctx.os.arch == "amd64"):
        rctx.download_and_extract(
            "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-x86_64-linux.tar.gz",
            output = "",
            sha256 = "0507679dff16814b74516cd969a9b16d2ced1347388024bc7966264648c78bfb",
            stripPrefix = "wasi-sdk-30.0-x86_64-linux",
        )

    # This results from bazel not being able to copy empty directories.
    rctx.file(
        "share/wasi-sysroot/include/c++/v1/nonexistent.txt",
    )

    rctx.file(
        "BUILD.bazel",
        content = """
package(default_visibility = ["//visibility:public"])


filegroup(name = "bin",     srcs = glob(["bin/**"]))
filegroup(name = "include", srcs = glob(["include/**"]))
filegroup(name = "lib",     srcs = glob(["lib/**"]))
filegroup(name = "share",   srcs = glob(["share/**"]))
        """,
    )

setup_wasi_deps = repository_rule(
    implementation = _setup_wasi_deps,
)
