load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

_WASI_SDK_EXEC_ARCH_ENV = "MONGO_WASI_SDK_EXEC_ARCH"
_LINUX_CROSS_TOOLCHAIN_ENV = "MONGO_LINUX_CROSS_TOOLCHAIN"

WASI_SDK_DIST = {
    ("linux", "aarch64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-arm64-linux.tar.gz",
        "sha256": "4f98ee738c7abb45c81a94d1461fc53cc569d1cd01498951c8184d841a027844",
        "stripPrefix": "wasi-sdk-33.0-arm64-linux",
    },
    ("linux", "amd64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-linux.tar.gz",
        "sha256": "0ba8b5bfaeb2adf3f29bab5841d76cf5318ab8e1642ea195f88baba1abd47bce",
        "stripPrefix": "wasi-sdk-33.0-x86_64-linux",
    },
    ("macos", "aarch64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-arm64-macos.tar.gz",
        "sha256": "85c997a2665ead91673b5bb88b7d0df3fc8900df3bfa244f720d478187bbdc78",
        "stripPrefix": "wasi-sdk-33.0-arm64-macos",
    },
    ("macos", "x86_64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-macos.tar.gz",
        "sha256": "18f3f201ba9734e6a4455b0b6410690395a55e9ffa9f6f5066f66083a94b93b3",
        "stripPrefix": "wasi-sdk-33.0-x86_64-macos",
    },
    ("windows", "amd64"): {
        "url": "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-33.0-x86_64-windows.tar.gz",
        "sha256": "df14ca2a2127c2d6b6be07e6f5549b3af9c1b3c0112430c200a4749970c59f06",
        "stripPrefix": "wasi-sdk-33.0-x86_64-windows",
    },
    ("linux", "s390x"): {
        "url": "https://mdb-build-public.s3.amazonaws.com/wasm-toolchain/435/wasi-sdk-33-s390x-rhel80-c10c050.tgz",
        "sha256": "77543f0a8a9d1a9c369f4a45a50504d85382cc4809d49d4fd66c65a1e0db6c45",
        "stripPrefix": "",
    },
    ("linux", "ppc64le"): {
        "url": "https://mdb-build-public.s3.amazonaws.com/wasm-toolchain/435/wasi-sdk-33-ppc64le-rhel81-c10c050.tgz",
        "sha256": "bd719f18c9b5d6daddc03b4f7098c3c835269cce97d056f406e226aa39a4fbc8",
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

def _normalize_execution_arch(arch, os):
    """Normalize an execution-platform architecture for the SDK archive."""
    arch = _normalize_arch(arch)
    if os == "linux" and arch == "x86_64":
        return "amd64"
    if os == "macos" and arch == "amd64":
        return "x86_64"
    return arch

def cross_execution_arch(selector):
    """Return the executable architecture encoded in a Linux cross selector.

    The cross-toolchain selector is already required by the Linux cross-RBE
    repository rules.  Using it as a fallback makes WASI selection robust when
    Bazel evaluates this repository before a command-line ``--repo_env`` value
    has been forwarded to the repository rule.
    """
    if not selector:
        return None

    target_and_exec = selector.split("_on_")
    if len(target_and_exec) != 2:
        return None

    execution_platform = target_and_exec[1]
    if execution_platform.endswith("_x86_64"):
        return "amd64"
    if execution_platform.endswith("_aarch64"):
        return "aarch64"
    return None

def wasi_execution_arch(rctx):
    """Select an SDK executable architecture, independent of the target CPU."""

    os = _normalize_os(rctx.os.name)

    # The explicit execution override is authoritative.  tools/bazel adds it
    # before bootstrap and the cross-RBE hook adds it to the final invocation,
    # so it remains deterministic even if a long-lived Bazel server has a
    # stale cross-toolchain selector in its environment.
    configured_arch = rctx.os.environ.get(_WASI_SDK_EXEC_ARCH_ENV, "")
    if configured_arch:
        return _normalize_execution_arch(configured_arch, os)

    # Keep the selector as a fallback for repository hydration paths that do
    # not receive the direct override (for example, an older caller invoking
    # the repository rule directly).
    cross_arch = cross_execution_arch(
        rctx.os.environ.get(_LINUX_CROSS_TOOLCHAIN_ENV, ""),
    )
    if cross_arch:
        return cross_arch

    # Without an explicit cross execution selector, repository hydration is
    # native. This is the path used by public-release-local on IBM hosts, where
    # WASI actions run in the host-native hermetic container. Do not silently
    # substitute an x86_64/aarch64 executable for a native IBM SDK.
    return _normalize_execution_arch(_normalize_arch(rctx.os.arch), os)

def _setup_wasi_deps(rctx):
    os = _normalize_os(rctx.os.name)
    arch = wasi_execution_arch(rctx)
    key = (os, arch)

    if key not in WASI_SDK_DIST:
        fail("Unsupported platform for wasi-sdk: os={}, arch={}".format(os, arch))

    dist = WASI_SDK_DIST[key]
    rctx.download_and_extract(
        dist["url"],
        output = "",
        sha256 = dist["sha256"],
        stripPrefix = dist["stripPrefix"],
    )

    if os == "linux" and rctx.os.arch in ("ppc64le", "s390x"):
        # CppArchive actions for the WASI SpiderMonkey libraries run in the
        # native IBM persistent container, while WASI compile/tool actions run
        # on the configured x86_64/aarch64 RBE execution platform.  A single
        # ELF llvm-ar cannot execute in both places. Keep the portable SDK
        # binary for RBE and dispatch to the native Mongo toolchain ar when the
        # action is actually running on an IBM host.
        move_result = rctx.execute([
            "mv",
            str(rctx.path("bin/llvm-ar")),
            str(rctx.path("bin/llvm-ar.sdk")),
        ])
        if move_result.return_code != 0:
            fail("Unable to prepare the portable WASI llvm-ar wrapper: {}".format(move_result.stderr))
        rctx.symlink(
            Label("@mongo_toolchain_v5//:v5/bin/llvm-ar"),
            "bin/llvm-ar.native",
        )
        rctx.file(
            "bin/llvm-ar",
            content = """#!/bin/sh
set -eu
case "$(uname -m)" in
    ppc64le|s390x)
        exec "$(dirname "$0")/llvm-ar.native" "$@"
        ;;
    *)
        exec "$(dirname "$0")/llvm-ar.sdk" "$@"
        ;;
esac
""",
            executable = True,
        )

    # Keep the repository key tied to the executable-selection contract.  The
    # SDK contains host executables, so reusing a repository hydrated before a
    # cross-execution setting changed can leave an IBM-target binary in an
    # amd64 RBE action and fail with ``Exec format error``.  The marker also
    # makes the selected rule version visible when diagnosing a sandbox.
    rctx.file("SELECTION_VERSION", rctx.attr.selection_version + "\n")

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
    attrs = {
        "selection_version": attr.string(
            mandatory = True,
            doc = "Version of the SDK executable-selection contract.",
        ),
    },
    # The selected SDK is executable tooling, so this repository must be
    # re-evaluated when the cross execution platform changes even though the
    # target platform remains wasm32.
    configure = True,
    environ = [_LINUX_CROSS_TOOLCHAIN_ENV, _WASI_SDK_EXEC_ARCH_ENV],
)
