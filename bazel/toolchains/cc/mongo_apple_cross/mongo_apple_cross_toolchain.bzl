"""Repository rules for MongoDB's macOS cross-compilation toolchain.

Two repos are created:

  1. `mongo_apple_cross_toolchain` — lightweight, always fetched.  Contains only
     `toolchain()` registration targets with platform constraints.  No downloads.

  2. `mongo_apple_cross_toolchain_files` — lazily fetched (only when Bazel selects
     the macOS cross-compilation toolchain).  Downloads LLVM + macOS SDK from S3
     (or uses local overrides via LLVM_PATH / MACOS_SDK_PATH env vars), then
     generates cc_toolchain / cc_toolchain_config targets.

This split ensures that normal Linux builds never pay the download cost.
"""

load("@build_bazel_apple_support//configs:platforms.bzl", "APPLE_PLATFORMS_CONSTRAINTS")

# S3 bucket and paths for pre-built toolchain archives
_S3_BUCKET = "https://mdb-build-public.s3.amazonaws.com"
_S3_PREFIX = "macos_cross_compile"

# LLVM toolchain archive (stripped to only what cross-compilation needs)
_LLVM_URL = _S3_BUCKET + "/" + _S3_PREFIX + "/llvm-22.1.1-cross-linux-arm64.tar.xz"
_LLVM_SHA256 = "172159dbf9db36f6a45e0836126213a354be9d784b3717faeec27b9e5e905af9"
_LLVM_STRIP_PREFIX = "LLVM-22.1.1-Linux-ARM64"
_LLVM_VERSION = "22"

# macOS SDK archive
_MACOS_SDK_URL = _S3_BUCKET + "/" + _S3_PREFIX + "/macos-sdk-15.2.tar.xz"
_MACOS_SDK_SHA256 = "b090a2bd6b0566616da8bdb9a88ab84e842fd3f44ff4be6a3d795a599d462a0e"
_MACOS_SDK_STRIP_PREFIX = "MacOSX15.2.sdk"

# ---------------------------------------------------------------------------
# Shared constants
# ---------------------------------------------------------------------------

# Map of supported architectures for cross-compilation
_CROSS_COMPILE_ARCHS = {
    "darwin_arm64": "arm64",
    "darwin_x86_64": "x86_64",
}

def get_supported_apple_cross_archs():
    """Returns a map of supported Apple architectures for cross-compilation."""
    return _CROSS_COMPILE_ARCHS

# ---------------------------------------------------------------------------
# Repo 1: lightweight toolchain registration (always fetched, no downloads)
# ---------------------------------------------------------------------------

def _apple_cross_toolchain_registration_impl(repository_ctx):
    """Generates toolchain() targets that point to the files repo."""
    os_name = repository_ctx.os.name.lower()
    if "linux" not in os_name and "mac" not in os_name:
        repository_ctx.file(
            "BUILD.bazel",
            "# Apple cross-compilation toolchain is only available from Linux or macOS\n",
        )
        return

    entries = [
        'package(default_visibility = ["//visibility:public"])\n',
    ]
    for arch, cpu in _CROSS_COMPILE_ARCHS.items():
        entries.append("""
toolchain(
    name = "mongo_apple_cross_{arch}_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:aarch64",
    ],
    target_compatible_with = [
        "@platforms//os:macos",
        "@platforms//cpu:{cpu}",
        "@//bazel/platforms:use_mongo_apple_cross_toolchain",
    ],
    toolchain = "@mongo_apple_cross_toolchain_files//:cc-compiler-apple-cross-{arch}",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
toolchain(
    name = "mongo_apple_cross_{arch}_macos_host_toolchain",
    exec_compatible_with = [
        "@platforms//os:macos",
    ],
    target_compatible_with = [
        "@platforms//os:macos",
        "@platforms//cpu:{cpu}",
        "@//bazel/platforms:use_mongo_apple_cross_toolchain",
    ],
    toolchain = "@mongo_apple_cross_toolchain_files//:cc-compiler-apple-cross-host-wrapper-{arch}",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(arch = arch, cpu = cpu))

    repository_ctx.file("BUILD.bazel", "\n".join(entries))

mongo_apple_cross_toolchain_config = repository_rule(
    implementation = _apple_cross_toolchain_registration_impl,
    configure = True,
)

# ---------------------------------------------------------------------------
# Repo 2: actual toolchain files (lazily fetched — only on cross-compile)
# ---------------------------------------------------------------------------

def _download_llvm(repository_ctx):
    """Downloads and extracts the LLVM toolchain, returning its path."""
    repository_ctx.report_progress("Downloading LLVM cross-compilation toolchain")
    repository_ctx.download_and_extract(
        url = _LLVM_URL,
        sha256 = _LLVM_SHA256,
        output = "llvm",
        stripPrefix = _LLVM_STRIP_PREFIX,
    )
    return str(repository_ctx.path("llvm"))

def _download_macos_sdk(repository_ctx):
    """Downloads and extracts the macOS SDK, returning its path."""
    repository_ctx.report_progress("Downloading macOS SDK")
    repository_ctx.download_and_extract(
        url = _MACOS_SDK_URL,
        sha256 = _MACOS_SDK_SHA256,
        output = "sdk",
        stripPrefix = _MACOS_SDK_STRIP_PREFIX,
    )
    return str(repository_ctx.path("sdk"))

def _get_llvm_path(repository_ctx):
    """Gets the LLVM path: uses LLVM_PATH env var if set, otherwise downloads from S3."""
    llvm_path = repository_ctx.os.environ.get("LLVM_PATH", "")
    if llvm_path:
        return llvm_path, None, False

    # Download from S3 (statically-linked LLVM with all needed tools)
    path = _download_llvm(repository_ctx)
    return path, None, True

def _get_apple_sdk_path(repository_ctx):
    """Gets the Apple SDK path: uses MACOS_SDK_PATH env var if set, otherwise downloads from S3."""
    sdk_path = repository_ctx.os.environ.get("MACOS_SDK_PATH", "")
    if sdk_path:
        return sdk_path, None, False

    sdk_path = repository_ctx.os.environ.get("SDKROOT", "")
    if sdk_path:
        return sdk_path, None, False

    # Download from S3 as default
    path = _download_macos_sdk(repository_ctx)
    return path, None, True

def _get_llvm_version(repository_ctx, llvm_path, downloaded):
    """Gets the LLVM version number."""
    if downloaded:
        return _LLVM_VERSION

    result = repository_ctx.execute([llvm_path + "/bin/clang", "--version"])
    if result.return_code == 0:
        for line in result.stdout.split("\n"):
            if "version" in line.lower():
                parts = line.split(" ")
                for i, part in enumerate(parts):
                    if part.lower() == "version" and i + 1 < len(parts):
                        version_str = parts[i + 1]
                        return version_str.split(".")[0]
    return "14"  # Default fallback

# LLVM tool binaries to symlink into the repo for hermetic builds
_LLVM_TOOLS = [
    "clang",
    "clang++",
    "clang-cpp",
    "ld.lld",
    "ld64.lld",
    "lld",
    "llvm-ar",
    "llvm-nm",
    "llvm-objcopy",
    "llvm-objdump",
    "llvm-strip",
    "llvm-dwp",
    "llvm-profdata",
    "llvm-cov",
]

_WRAPPED_TOOL_PATHS = {
    "ar": "llvm-ar",
    "cpp": "clang-cpp",
    "ld": "ld.lld",
    "dwp": "llvm-dwp",
    "gcc": "clang",
    "g++": "clang++",
    "gcov": "llvm-profdata",
    "llvm-cov": "llvm-cov",
    "llvm-profdata": "llvm-profdata",
    "nm": "llvm-nm",
    "objcopy": "llvm-objcopy",
    "objdump": "llvm-objdump",
    "strip": "llvm-strip",
}

_WRAPPER_TEMPLATE = """#!/usr/bin/env bash
set -euo pipefail
this_dir="$(cd "$(dirname "${{BASH_SOURCE[0]}}")" && pwd -P)"
real_tool="${{this_dir}}/../tools/{real_tool}"
if [[ "$(uname -s)" == "Linux" ]]; then
  exec "${{real_tool}}" "$@"
fi
if [[ "${{MONGO_MACOS_CROSS_ACTION_WRAPPER:-1}}" == "0" ]]; then
  exec "${{real_tool}}" "$@"
fi
wrapper="${{MONGO_MACOS_CROSS_ACTION_WRAPPER_SCRIPT:-bazel/toolchains/cc/mongo_apple_cross/macos_cross_action_wrapper.py}}"
python="${{MONGO_MACOS_CROSS_ACTION_PYTHON:-python3}}"
exec "${{python}}" "${{wrapper}}" "${{real_tool}}" "$@"
"""

def _symlink_toolchain_files(repository_ctx, llvm_path, llvm_version, sdk_path):
    """Symlinks LLVM tools, headers, and SDK into the repo for hermetic builds."""

    # Symlink LLVM tool binaries
    for tool in _LLVM_TOOLS:
        src = llvm_path + "/bin/" + tool
        if repository_ctx.execute(["test", "-e", src]).return_code == 0:
            repository_ctx.symlink(src, "tools/" + tool)

    # Ensure ld64.lld exists — clang looks for it (not ld.lld) when targeting
    # macOS with -fuse-ld=lld.  LLD is a multi-format linker; it picks the
    # Mach-O backend automatically when invoked as "ld64.lld".
    ld64_lld = repository_ctx.path("tools/ld64.lld")
    if not ld64_lld.exists:
        lld = repository_ctx.path("tools/lld")
        if lld.exists:
            repository_ctx.symlink(lld, "tools/ld64.lld")

    # Symlink clang as a versioned name too (some internal lookups need it)
    repository_ctx.symlink(llvm_path + "/bin/clang", "tools/clang-" + llvm_version)

    # Symlink LLVM libc++ headers (needed for C++ standard library).
    repository_ctx.symlink(llvm_path + "/include/c++/v1", "include/c++/v1")

    # Override __config_site to enable vendor availability annotations.
    config_site_content = repository_ctx.read(llvm_path + "/include/c++/v1/__config_site")
    config_site_content = config_site_content.replace(
        "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0",
        "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 1",
    )
    repository_ctx.file("include/config_override/__config_site", config_site_content)

    # Symlink Clang builtin headers (needed for stddef.h, stdarg.h, etc.)
    repository_ctx.symlink(
        llvm_path + "/lib/clang/" + llvm_version + "/include",
        "include/clang",
    )

    # Symlink macOS SDK directories
    repository_ctx.symlink(sdk_path + "/usr/include", "sysroot/usr/include")
    repository_ctx.symlink(sdk_path + "/usr/lib", "sysroot/usr/lib")
    repository_ctx.symlink(sdk_path + "/System", "sysroot/System")

def _write_host_wrapper_scripts(repository_ctx):
    """Writes host-executable tool wrappers for local macOS host actions."""
    wrapper_names = []
    for wrapper_name in _WRAPPED_TOOL_PATHS.values():
        if wrapper_name in wrapper_names:
            continue
        wrapper_names.append(wrapper_name)
    for wrapper_name in wrapper_names:
        repository_ctx.file(
            "wrappers/" + wrapper_name,
            _WRAPPER_TEMPLATE.format(real_tool = wrapper_name),
            executable = True,
        )

def _configure_cross_toolchain(repository_ctx):
    """Configures the cross-compilation toolchain files."""
    build_file = "BUILD.bazel"

    # Get LLVM path (downloads from S3 if no local override)
    llvm_path, error, llvm_downloaded = _get_llvm_path(repository_ctx)
    if error:
        return False, error

    # Get Apple SDK path (downloads from S3 if no local override)
    sdk_path, error, sdk_downloaded = _get_apple_sdk_path(repository_ctx)
    if error:
        return False, error

    # Get LLVM version
    llvm_version = _get_llvm_version(repository_ctx, llvm_path, llvm_downloaded)

    # Get minimum macOS version
    min_macos_version = repository_ctx.os.environ.get("MACOS_MIN_VERSION", "14.0")

    # Validate minimum macOS version is at least 14
    min_version_major = int(min_macos_version.split(".")[0])
    if min_version_major < 14:
        fail("MACOS_MIN_VERSION must be at least 14.0, got: " + min_macos_version)

    # Symlink LLVM tools, headers, and SDK into the repo for hermetic builds.
    repository_ctx.report_progress("Symlinking LLVM tools and macOS SDK for hermetic toolchain")
    _symlink_toolchain_files(repository_ctx, llvm_path, llvm_version, sdk_path)
    _write_host_wrapper_scripts(repository_ctx)

    # Compute the execroot-relative path prefix for this external repository.
    repo_name = repository_ctx.name
    execroot_prefix = "external/" + repo_name

    repository_ctx.report_progress("Generating macOS cross-compilation toolchain build file")
    build_template = Label("@//bazel/toolchains/cc/mongo_apple_cross:BUILD.tmpl")
    repository_ctx.template(
        build_file,
        build_template,
        {
            "%{llvm_version}": llvm_version,
            "%{min_macos_version}": min_macos_version,
            "%{execroot_prefix}": execroot_prefix,
        },
    )

    return True, ""

def _apple_cross_toolchain_files_impl(repository_ctx):
    """Downloads and configures the actual toolchain files."""
    os_name = repository_ctx.os.name.lower()
    if "linux" not in os_name and "mac" not in os_name:
        repository_ctx.file(
            "BUILD.bazel",
            "# Apple cross-compilation toolchain is only available from Linux or macOS\n",
        )
        return

    success, error_msg = _configure_cross_toolchain(repository_ctx)
    if not success:
        repository_ctx.file(
            "BUILD.bazel",
            """# Apple cross-compilation toolchain configuration failed:
# {}

# To enable macOS cross-compilation, either:
# 1. Just run: bazel build --config=macos-cross-arm64 (downloads LLVM + SDK from S3)
# 2. Or override with local paths: set LLVM_PATH and/or MACOS_SDK_PATH env vars
""".format(error_msg.replace("\n", "\n# ")),
        )

mongo_apple_cross_toolchain_files_config = repository_rule(
    environ = [
        "LLVM_PATH",
        "MACOS_SDK_PATH",
        "SDKROOT",
        "MACOS_MIN_VERSION",
    ],
    implementation = _apple_cross_toolchain_files_impl,
    configure = True,
)

# ---------------------------------------------------------------------------
# Module extension: sets up both repos
# ---------------------------------------------------------------------------

def setup_mongo_apple_cross_toolchain():
    """Sets up the macOS cross-compilation toolchain (both repos)."""
    mongo_apple_cross_toolchain_config(
        name = "mongo_apple_cross_toolchain",
    )
    mongo_apple_cross_toolchain_files_config(
        name = "mongo_apple_cross_toolchain_files",
    )

setup_mongo_apple_cross_toolchain_extension = module_extension(
    implementation = lambda ctx: setup_mongo_apple_cross_toolchain(),
)
