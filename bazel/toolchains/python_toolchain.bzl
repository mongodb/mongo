"""Repository rules for rules_py_simple"""

_OS_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}

_ARCH_MAP = {
    "amd64": "@platforms//cpu:x86_64",
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
}

URLS_MAP = {
        "linux_aarch64":{
            "sha": "c7573fdb00239f86b22ea0e8e926ca881d24fde5e5890851339911d76110bc35",
            "url": "https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-aarch64-unknown-linux-gnu-install_only.tar.gz",
            "interpreter_path": "bin/python3",
        },
        "linux_amd64":{
            "sha": "c5bcaac91bc80bfc29cf510669ecad12d506035ecb3ad85ef213416d54aecd79",
            "url": "https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-x86_64-unknown-linux-gnu-install_only.tar.gz",
            "interpreter_path": "bin/python3",
        },
        "windows_amd64":{
            "sha": "97ebca93a928802f421387dcc6ec5403a3e513f43c2df35b7c3e3cca844d79d0",
            "url": "https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-x86_64-pc-windows-msvc-static-install_only.tar.gz",
            "interpreter_path": "python3.exe",
        },
        "macos_aarch64":{
            "sha": "8348bc3c2311f94ec63751fb71bd0108174be1c4def002773cf519ee1506f96f",
            "url": "https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-aarch64-apple-darwin-install_only.tar.gz",
            "interpreter_path": "bin/python3",
        },
        "macos_x86_64":{
            "sha": "bd3fc6e4da6f4033ebf19d66704e73b0804c22641ddae10bbe347c48f82374ad",
            "url": "https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-x86_64-apple-darwin-install_only.tar.gz",
            "interpreter_path": "bin/python3",
        }

    }

def _py_download(ctx):
    """
    Downloads and builds a Python distribution.

    Args:
        ctx: Repository context.
    """


    if ctx.attr.os:
        os = ctx.attr.os
    else:
        if "win" in ctx.os.name:
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
        platform_info = URLS_MAP["{os}_{arch}".format(os=os, arch=arch)]
        urls = platform_info['url']
        sha = platform_info['sha']
        interpreter_path = platform_info['interpreter_path']

    ctx.report_progress("downloading python")
    ctx.download_and_extract(
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
            values = ["amd64", "aarch64"],
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
        sha256 = "c7573fdb00239f86b22ea0e8e926ca881d24fde5e5890851339911d76110bc35",
        urls = ["https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-aarch64-unknown-linux-gnu-install_only.tar.gz"],
    )

    py_download(
        name = "py_linux_x86_64",
        arch = "amd64",
        os = "linux",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = "c5bcaac91bc80bfc29cf510669ecad12d506035ecb3ad85ef213416d54aecd79",
        urls = ["https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-x86_64-unknown-linux-gnu-install_only.tar.gz"],
    )

    py_download(
        name = "py_windows_x86_64",
        arch = "amd64",
        os = "windows",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        interpreter_path = "python.exe",
        sha256 = "97ebca93a928802f421387dcc6ec5403a3e513f43c2df35b7c3e3cca844d79d0",
        urls = ["https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-x86_64-pc-windows-msvc-static-install_only.tar.gz"],
    )

    py_download(
        name = "py_macos_arm64",
        arch = "aarch64",
        os = "macos",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 = "8348bc3c2311f94ec63751fb71bd0108174be1c4def002773cf519ee1506f96f",
        urls = ["https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-aarch64-apple-darwin-install_only.tar.gz"],
    )

    py_download(
        name = "py_macos_x86_64",
        arch = "amd64",
        os = "macos",
        build_tpl = "//bazel/toolchains:python_toolchain.BUILD",
        sha256 ="bd3fc6e4da6f4033ebf19d66704e73b0804c22641ddae10bbe347c48f82374ad",
        urls = ["https://github.com/indygreg/python-build-standalone/releases/download/20230507/cpython-3.10.11+20230507-x86_64-apple-darwin-install_only.tar.gz"],
    )

    return (
        "@py_linux_arm64//:python_toolchain", 
        "@py_linux_x86_64//:python_toolchain",
        "@py_windows_x86_64//:python_toolchain",
        "@py_macos_arm64//:python_toolchain", 
        "@py_macos_x86_64//:python_toolchain",
    )
    