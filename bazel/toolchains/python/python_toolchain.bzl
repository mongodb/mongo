"""Repository rules for rules_py_simple"""

load("//bazel:utils.bzl", "retry_download_and_extract", "write_python_pyc_cache_prefix_customization")
load("//bazel/platforms:normalize.bzl", "ARCH_TO_PLATFORM_MAP", "OS_TO_PLATFORM_MAP")

URLS_MAP = {
    "linux_aarch64": {
        "sha": "d9a1344428f16f055d98ba8885b2dd96d780fcb761a3322fdfdadbaf04290d31",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-aarch64-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "dist/bin/python3",
    },
    "linux_amd64": {
        "sha": "f31a96948bacdb8155cb3bca643fce47014f9610d90c8f7dcd62973452a43ff5",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-x86_64-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "dist/bin/python3",
    },
    "linux_ppc64le": {
        "sha": "e1e406591f346d98f029c0e117130b4307fa802b98da9b82191e43ed2108b5ce",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-ppc64le-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "dist/bin/python3",
    },
    "linux_s390x": {
        "sha": "27d6e8219687eae81a3a09ae2819e32bbb80aa8ba0f03ae953697068a3f9011c",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-s390x-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "dist/bin/python3",
    },
    "windows_amd64": {
        "sha": "9b8e1b811ce8fdf3073e7ea001db448504c8628404b30b676771d52a3686e355",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-x86_64-pc-windows-msvc-install_only.tar.gz",
        "interpreter_path": "dist/python.exe",
    },
    "macos_aarch64": {
        "sha": "229b1bd07558477db0ad9db1168d7e77c6b3df8284201919305af6d603340a27",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-aarch64-apple-darwin-install_only.tar.gz",
        "interpreter_path": "dist/bin/python3",
    },
    "macos_x86_64": {
        "sha": "47d438bbf7b912d8f19ff4436a514f8529e5736ca38cb4bc94ae25d3b2384f15",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20260114/cpython-3.13.11+20260114-x86_64-apple-darwin-install_only.tar.gz",
        "interpreter_path": "dist/bin/python3",
    },
}

def _py_download(ctx):
    """
    Downloads and builds a Python distribution.

    Args:
        ctx: Repository context.
    """

    if ctx.attr.os:
        os = ctx.attr.os
    elif "win" in ctx.os.name:
        os = "windows"
    elif "mac" in ctx.os.name:
        os = "macos"
    else:
        os = "linux"

    if ctx.attr.arch:
        arch = ctx.attr.arch
    else:
        arch = ctx.os.arch

    # Platform-conditional download: only download if this toolchain matches the host OS
    # This prevents downloading all 7 platform toolchains (saves ~630MB per build)
    host_os_name = ctx.os.name.lower()
    is_host_windows = "win" in host_os_name
    is_host_macos = "mac" in host_os_name or "darwin" in host_os_name
    is_host_linux = not is_host_windows and not is_host_macos

    is_toolchain_windows = os == "windows"
    is_toolchain_macos = os == "macos"
    is_toolchain_linux = os == "linux"

    # Check if OS matches
    os_matches = (
        (is_host_windows and is_toolchain_windows) or
        (is_host_macos and is_toolchain_macos) or
        (is_host_linux and is_toolchain_linux)
    )

    # If OS doesn't match, create a minimal stub BUILD file and skip download
    if not os_matches:
        os_constraint = OS_TO_PLATFORM_MAP[os]
        arch_constraint = ARCH_TO_PLATFORM_MAP[arch]
        constraints = [os_constraint, arch_constraint]
        constraints_str = ",\n        ".join(['"%s"' % c for c in constraints])

        ctx.file("BUILD.bazel", """
# Stub toolchain - platform doesn't match host, not downloaded
load("@bazel_tools//tools/python:toolchain.bzl", "py_runtime_pair")

py_runtime_pair(
    name = "runtime_pair",
    py2_runtime = None,
    py3_runtime = None,
)

toolchain(
    name = "python_toolchain",
    toolchain_type = "@bazel_tools//tools/python:toolchain_type",
    toolchain = ":runtime_pair",
    exec_compatible_with = [
        {constraints}
    ],
    visibility = ["//visibility:public"],
)
""".format(constraints = constraints_str))
        return None

    if ctx.attr.urls:
        urls = ctx.attr.urls
        sha = ctx.attr.sha256
        interpreter_path = ctx.attr.interpreter_path
    else:
        platform_info = URLS_MAP["{os}_{arch}".format(os = os, arch = arch)]
        urls = platform_info["url"]
        sha = platform_info["sha"]
        interpreter_path = platform_info["interpreter_path"]

    ctx.report_progress("downloading python")
    retry_download_and_extract(
        ctx = ctx,
        output = "dist",
        tries = 5,
        url = urls,
        sha256 = sha,
        stripPrefix = "python",
    )

    windows_python = False
    for name in ctx.path("dist").readdir():
        if name.basename == "python.exe":
            windows_python = True
            break

    if windows_python:
        # windows does not have python version specific dir
        usercustomize_file = "dist/Lib/site-packages/usercustomize.py"
    else:
        # detect python version without execution
        # this looks for the `python#.#` binary on macos and linux
        # and extracts the version information at the end of the binary,
        # starlark doesn't have regex support so had to roll our own
        # parsing.
        python_base_dir = ctx.path("dist/bin")
        bin_files = python_base_dir.readdir()
        python_major_version = -1
        python_minor_version = -1
        for bin_file in bin_files:
            basename = bin_file.basename
            if basename.startswith("python"):
                numeric = basename.replace("python", "")
                versions = numeric.split(".")
                if len(versions) == 2:
                    numbers_only = True
                    for version in versions:
                        if not version.isdigit():
                            numbers_only = False
                    if numbers_only:
                        python_major_version = versions[0]
                        python_minor_version = versions[1]
                        break

        if python_major_version == -1 or python_minor_version == -1:
            ctx.fail("Could not detect python versions")

        usercustomize_file = "dist/lib/python" + python_major_version + "." + python_minor_version + "/site-packages/usercustomize.py"

    write_python_pyc_cache_prefix_customization(ctx, usercustomize_file)

    ctx.report_progress("generating build file")
    os_constraint = OS_TO_PLATFORM_MAP[os]
    arch_constraint = ARCH_TO_PLATFORM_MAP[arch]

    constraints = [os_constraint, arch_constraint]

    # So Starlark doesn't throw an indentation error when this gets injected.
    constraints_str = ",\n        ".join(['"%s"' % c for c in constraints])

    # Inject our string substitutions into the BUILD file template, and drop said BUILD file in the WORKSPACE root of the repository.
    substitutions = {
        "{constraints}": constraints_str,
        "{interpreter_path}": interpreter_path,
    }

    if os == "windows":
        # Read-only secures the toolchain but on windows makes bazel unable to clean or reinstall it
        #ctx.execute(['icacls', 'dist', '/inheritance:r', '/grant:r', 'Everyone:R', '/T'])
        #ctx.execute(['icacls', 'dist', '/inheritance:r', '/grant:r', 'Administrators:R', '/T'])
        pass
    else:
        ctx.execute(["chmod", "-R", "544", "dist"])

    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions = substitutions,
    )

    return None

py_download = repository_rule(
    implementation = _py_download,
    attrs = {
        "urls": attr.string_list(
            doc = "String list of mirror URLs where the Python distribution can be downloaded.",
        ),
        "sha256": attr.string(
            doc = "Expected SHA-256 sum of the archive.",
        ),
        "os": attr.string(
            values = ["macos", "linux", "windows"],
            doc = "Host operating system.",
        ),
        "arch": attr.string(
            values = ["amd64", "aarch64", "ppc64le", "s390x"],
            doc = "Host architecture.",
        ),
        "interpreter_path": attr.string(
            default = "dist/bin/python3",
            doc = "Path you'd expect the python interpreter binary to live.",
        ),
        "build_tpl": attr.label(
            default = "//bazel/toolchains/python:python_toolchain.BUILD.tmpl",
            doc = "Label denoting the BUILD file template that get's installed in the repo.",
        ),
    },
)

def setup_mongo_python_toolchains(ctx):
    """Setup Python toolchains for all platforms.

    Creates py_download repos for all platforms. The py_download repository rule
    detects if the toolchain OS matches the host OS and only downloads if there's
    a match. Non-matching platforms get stub BUILD files without downloading.

    This ensures MODULE.bazel.lock is platform-agnostic since all platforms see the
    same repository definitions.

    Args:
        ctx: Module extension context (unused, required by signature).
    """

    # Always create py_host for local repository operations
    py_download(name = "py_host")

    # Create all platform toolchains - downloads are conditional inside py_download
    py_download(
        name = "py_linux_arm64",
        arch = "aarch64",
        os = "linux",
        sha256 = URLS_MAP["linux_aarch64"]["sha"],
        urls = [URLS_MAP["linux_aarch64"]["url"]],
    )

    py_download(
        name = "py_linux_x86_64",
        arch = "amd64",
        os = "linux",
        sha256 = URLS_MAP["linux_amd64"]["sha"],
        urls = [URLS_MAP["linux_amd64"]["url"]],
    )

    py_download(
        name = "py_linux_ppc64le",
        arch = "ppc64le",
        os = "linux",
        sha256 = URLS_MAP["linux_ppc64le"]["sha"],
        urls = [URLS_MAP["linux_ppc64le"]["url"]],
    )

    py_download(
        name = "py_linux_s390x",
        arch = "s390x",
        os = "linux",
        sha256 = URLS_MAP["linux_s390x"]["sha"],
        urls = [URLS_MAP["linux_s390x"]["url"]],
    )

    py_download(
        name = "py_windows_x86_64",
        arch = "amd64",
        os = "windows",
        interpreter_path = "dist/python.exe",
        sha256 = URLS_MAP["windows_amd64"]["sha"],
        urls = [URLS_MAP["windows_amd64"]["url"]],
    )

    py_download(
        name = "py_macos_arm64",
        arch = "aarch64",
        os = "macos",
        sha256 = URLS_MAP["macos_aarch64"]["sha"],
        urls = [URLS_MAP["macos_aarch64"]["url"]],
    )

    py_download(
        name = "py_macos_x86_64",
        arch = "amd64",
        os = "macos",
        sha256 = URLS_MAP["macos_x86_64"]["sha"],
        urls = [URLS_MAP["macos_x86_64"]["url"]],
    )

    # Return value is not used since toolchains are registered in MODULE.bazel
    return (
        "@py_linux_arm64//:python_toolchain",
        "@py_linux_x86_64//:python_toolchain",
        "@py_linux_ppc64le//:python_toolchain",
        "@py_linux_s390x//:python_toolchain",
        "@py_windows_x86_64//:python_toolchain",
        "@py_macos_arm64//:python_toolchain",
        "@py_macos_x86_64//:python_toolchain",
    )
