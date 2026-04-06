load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

_WASI_SDK_DIST = {
    ("linux", "aarch64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-arm64-linux.tar.gz",
        "sha256": "6f2977942308d91b0123978da3c6a0d6fce780994b3b020008c617e26764ea40",
        "stripPrefix": "wasi-sdk-30.0-arm64-linux",
    },
    ("linux", "amd64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-x86_64-linux.tar.gz",
        "sha256": "0507679dff16814b74516cd969a9b16d2ced1347388024bc7966264648c78bfb",
        "stripPrefix": "wasi-sdk-30.0-x86_64-linux",
    },
    ("macos", "aarch64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-arm64-macos.tar.gz",
        "sha256": "2c2ed99296857e60fd14c3f40fe226231f296409502491094704089c31a16740",
        "stripPrefix": "wasi-sdk-30.0-arm64-macos",
    },
    ("macos", "x86_64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-x86_64-macos.tar.gz",
        "sha256": "1594a0791309781bf0d0224431c3556ec4a2326b205687b659f6550d08d8b13e",
        "stripPrefix": "wasi-sdk-30.0-x86_64-macos",
    },
    ("windows", "amd64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-30/wasi-sdk-30.0-x86_64-windows.tar.gz",
        "sha256": "e87d6bf9f9ca3482a75f1cbc630f095b4ae8c98d586708bac7adf08c03b327bc",
        "stripPrefix": "wasi-sdk-30.0-x86_64-windows",
    },
    ("linux", "s390x"): {
        "url": "https://mdb-build-public.s3.amazonaws.com/wasm-toolchain/418/wasi-sdk-30-s390x-rhel80-3d4ea12.tgz",
        "sha256": "c31c661cc49b7b99e092b3bb5d7365042f9fbeb5495c9ec34d01b096f011e8f2",
        "stripPrefix": "",
    },
    ("linux", "ppc64le"): {
        "url": "https://mdb-build-public.s3.amazonaws.com/wasm-toolchain/420/wasi-sdk-30-ppc64le-rhel81-3d4ea12.tgz",
        "sha256": "a7ee9e3760dc8cafea9557d1d4fbc8fc2a35ed4cf29a13dd47b8025d5004c57a",
        "stripPrefix": "",
    },
}

def _normalize_os(name):
    if name.startswith("mac os"):
        return "macos"
    if name.startswith("windows"):
        return "windows"
    return "linux"

def _normalize_arch(arch):
    if arch == "arm64":
        return "aarch64"
    return arch

def _setup_wasi_deps(rctx):
    os = _normalize_os(rctx.os.name)
    arch = _normalize_arch(rctx.os.arch)
    key = (os, arch)

    if key not in _WASI_SDK_DIST:
        fail("Unsupported platform for wasi-sdk: os={}, arch={}".format(os, arch))

    dist = _WASI_SDK_DIST[key]
    rctx.download_and_extract(
        dist["url"],
        output = "",
        sha256 = dist["sha256"],
        stripPrefix = dist["stripPrefix"],
    )

    # This results from bazel not being able to copy empty directories.
    rctx.file(
        "share/wasi-sysroot/include/c++/v1/nonexistent.txt",
    )

    # On Windows the binaries have .exe suffixes. Create wrapper scripts so
    # the toolchain config can use the same label on all platforms.
    exe = ".exe" if os == "windows" else ""

    rctx.file(
        "BUILD.bazel",
        content = """
package(default_visibility = ["//visibility:public"])

filegroup(name = "bin",     srcs = glob(["bin/**"]))
filegroup(name = "include", srcs = glob(["include/**"]))
filegroup(name = "lib",     srcs = glob(["lib/**"]))
filegroup(name = "share",   srcs = glob(["share/**"]))

# Platform-independent aliases for the WASI SDK tools so the toolchain
# config can use the same labels on Linux, macOS, and Windows.
alias(name = "wasm32-wasip2-clang",   actual = "bin/wasm32-wasip2-clang{exe}")
alias(name = "wasm32-wasip2-clang++", actual = "bin/wasm32-wasip2-clang++{exe}")
alias(name = "llvm-ar",               actual = "bin/llvm-ar{exe}")
        """.format(exe = exe),
    )

setup_wasi_deps = repository_rule(
    implementation = _setup_wasi_deps,
)
