"""Repository rules for rules_py_simple"""

load("//bazel:utils.bzl", "retry_download_and_extract")

_OS_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}

_ARCH_MAP = {
    "amd64": "@platforms//cpu:x86_64",
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
    "ppc64le": "@platforms//cpu:ppc64le",
    "s390x": "@platforms//cpu:s390x",
}

URLS_MAP = {
    "linux_aarch64": {
        "sha": "3e26a672df17708c4dc928475a5974c3fb3a34a9b45c65fb4bd1e50504cc84ec",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-aarch64-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "bin/python3",
    },
    "linux_amd64": {
        "sha": "ee37a7eae6e80148c7e3abc56e48a397c1664f044920463ad0df0fc706eacea8",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-x86_64-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "bin/python3",
    },
    "linux_ppc64le": {
        "sha": "7937035f690a624dba4d014ffd20c342e843dd46f89b0b0a1e5726b85deb8eaf",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-ppc64le-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "bin/python3",
    },
    "linux_s390x": {
        "sha": "f9f19823dba3209cedc4647b00f46ed0177242917db20fb7fb539970e384531c",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-s390x-unknown-linux-gnu-install_only.tar.gz",
        "interpreter_path": "bin/python3",
    },
    "windows_amd64": {
        "sha": "35458ef3163a2705cd0952ba1df6012acb42b043349dcb31ab49afec341369cf",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-x86_64-pc-windows-msvc-static-install_only.tar.gz",
        "interpreter_path": "python3.exe",
    },
    "macos_aarch64": {
        "sha": "916c35125b5d8323a21526d7a9154ca626453f63d0878e95b9f613a95006c990",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-aarch64-apple-darwin-install_only.tar.gz",
        "interpreter_path": "bin/python3",
    },
    "macos_x86_64": {
        "sha": "178cb1716c2abc25cb56ae915096c1a083e60abeba57af001996e8bc6ce1a371",
        "url": "https://github.com/indygreg/python-build-standalone/releases/download/20231002/cpython-3.11.6+20231002-x86_64-apple-darwin-install_only.tar.gz",
        "interpreter_path": "bin/python3",
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
        tries = 5,
        url = urls,
        sha256 = sha,
        stripPrefix = "python",
    )

    ctx.report_progress("generating build file")
    os_constraint = _OS_MAP[os]
    arch_constraint = _ARCH_MAP[arch]

    constraints = [os_constraint, arch_constraint]

    # So Starlark doesn't throw an indentation error when this gets injected.
    constraints_str = ",\n        ".join(['"%s"' % c for c in constraints])

    # Inject our string substitutions into the BUILD file template, and drop said BUILD file in the WORKSPACE root of the repository.
    substitutions = {
        "{constraints}": constraints_str,
        "{interpreter_path}": interpreter_path,
    }

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
            default = "bin/python3",
            doc = "Path you'd expect the python interpreter binary to live.",
        ),
        "build_tpl": attr.label(
            default = "//bazel/toolchains:python_toolchain.BUILD",
            doc = "Label denoting the BUILD file template that get's installed in the repo.",
        ),
    },
)

def setup_mongo_python_toolchains():
    # This will autoselect a toolchain that matches the host environment
    # this toolchain is intended be used only for local repository exectutions,
    # and will not be registered as a bazel toolchain by omitting from the return
    # value below.
    py_download(
        name = "py_host",
    )

    py_download(
        name = "py_linux_arm64",
        arch = "aarch64",
        os = "linux",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = URLS_MAP["linux_aarch64"]["sha"],
        urls = [URLS_MAP["linux_aarch64"]["url"]],
    )

    py_download(
        name = "py_linux_x86_64",
        arch = "amd64",
        os = "linux",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = URLS_MAP["linux_amd64"]["sha"],
        urls = [URLS_MAP["linux_amd64"]["url"]],
    )

    py_download(
        name = "py_linux_ppc64le",
        arch = "ppc64le",
        os = "linux",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = URLS_MAP["linux_ppc64le"]["sha"],
        urls = [URLS_MAP["linux_ppc64le"]["url"]],
    )

    py_download(
        name = "py_linux_s390x",
        arch = "s390x",
        os = "linux",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = URLS_MAP["linux_s390x"]["sha"],
        urls = [URLS_MAP["linux_s390x"]["url"]],
    )

    py_download(
        name = "py_windows_x86_64",
        arch = "amd64",
        os = "windows",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        interpreter_path = "python.exe",
        sha256 = URLS_MAP["windows_amd64"]["sha"],
        urls = [URLS_MAP["windows_amd64"]["url"]],
    )

    py_download(
        name = "py_macos_arm64",
        arch = "aarch64",
        os = "macos",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = URLS_MAP["macos_aarch64"]["sha"],
        urls = [URLS_MAP["macos_aarch64"]["url"]],
    )

    py_download(
        name = "py_macos_x86_64",
        arch = "amd64",
        os = "macos",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = URLS_MAP["macos_x86_64"]["sha"],
        urls = [URLS_MAP["macos_x86_64"]["url"]],
    )

    return (
        "@py_linux_arm64//:python_toolchain",
        "@py_linux_x86_64//:python_toolchain",
        "@py_linux_ppc64le//:python_toolchain",
        "@py_linux_s390x//:python_toolchain",
        "@py_windows_x86_64//:python_toolchain",
        "@py_macos_arm64//:python_toolchain",
        "@py_macos_x86_64//:python_toolchain",
    )
