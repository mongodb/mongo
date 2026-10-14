"""Repository rules for MongoDB's Windows cross-compilation toolchain.

The toolchain executes on Linux or through host wrappers and targets Windows
x86_64. It is intentionally separate from the native Windows/MSVC toolchain so
Windows developer builds keep their current host-local Visual Studio behavior.
"""

# LLVM archive for Windows cross-compilation. The binaries execute in the
# x86_64 Linux RBE action container and target Windows PE/COFF through clang-cl.
#
# The x86_64 archive is a repackaged subset of the official release
# (https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/LLVM-19.1.7-Linux-X64.tar.xz,
# sha256 4a5ec53951a584ed36f80240f6fbf8fdd46b4cf6c7ee87cc2d5018dc37caf679) made by
# package_minimal_llvm.py: 76MB instead of 1.6GB compressed, 420MB instead of 8.2GB unpacked.
# It holds only regular files; the multicall tools are stored under their real names
# (clang-19, lld, llvm-ar) and reached through _LLVM_TOOL_FALLBACKS.

_LLVM_X64_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/llvm-19.1.7-windows-cross-x86_64-linux.tar.xz"
_LLVM_X64_SHA256 = "95ff64067798030ee3cd16e2cb4e26c1bfbecc0591210b17b98f6164f1853b4d"
_LLVM_X64_STRIP_PREFIX = "llvm-19.1.7-windows-cross-x86_64-linux"

_SYSROOT_URL = ""
_SYSROOT_SHA256 = ""
_SYSROOT_STRIP_PREFIX = ""

_CROSS_COMPILE_ARCHS = {
    "windows_x86_64": "x64_windows",
}

_LLVM_TOOLS = [
    "clang",
    "clang++",
    "clang-cl",
    "lld-link",
    "llvm-lib",
    "llvm-rc",
]

# Tools to symbolize Windows binaries offline: both read PDBs natively, so they work on Linux
# and do not depend on dbghelp. Linked when present; local LLVM trees may not have them.
_OPTIONAL_LLVM_TOOLS = [
    "llvm-symbolizer",
    "llvm-pdbutil",
]

_LLVM_TOOL_FALLBACKS = {
    "clang": ["clang-19", "clang-20", "clang-21", "clang-22", "clang-18"],
    "clang++": ["clang-19", "clang-20", "clang-21", "clang-22", "clang-18"],
    # The official Linux LLVM archive does not ship a separate clang-cl
    # executable. Clang selects CL driver mode when invoked through a
    # clang-cl-named symlink, so fall back to clang itself; the minimal
    # archive has no clang symlink, only the versioned binary.
    "clang-cl": [
        "clang-cl-19",
        "clang-cl-20",
        "clang-cl-21",
        "clang-cl-22",
        "clang-cl-18",
        "clang",
        "clang-19",
        "clang-20",
        "clang-21",
        "clang-22",
        "clang-18",
    ],
    "lld-link": ["lld"],
    "llvm-lib": ["llvm-ar"],
}

_WRAPPED_TOOL_PATHS = {
    "ar": "llvm-lib",
    "cpp": "clang-cl",
    "gcc": "clang-cl",
    "gcov": "clang-cl",
    "ld": "lld-link",
    "ml": "clang-cl",
    "nm": "clang-cl",
    "objcopy": "clang-cl",
    "objdump": "clang-cl",
    "strip": "clang-cl",
}

_RC_CLANG_WRAPPER_TEMPLATE = """#!/bin/bash
set -euo pipefail
this_path="${BASH_SOURCE[0]}"
this_dir="${this_path%/*}"
if [[ "${this_dir}" == "${this_path}" ]]; then
  this_dir="."
fi

# llvm-rc runs clang as a subprocess. Windows SDK header names are resolved
# case-insensitively on Windows, but the RBE worker filesystem is Linux.
exec "${this_dir}/clang-real" \\
  -Xclang -ivfsoverlay \\
  -Xclang "${this_dir}/../case_insensitive_vfs.yaml" \\
  "$@"
"""

_WRAPPER_TEMPLATE = """#!/bin/bash
set -euo pipefail
this_path="${{BASH_SOURCE[0]}}"
this_dir="${{this_path%/*}}"
if [[ "${{this_dir}}" == "${{this_path}}" ]]; then
  this_dir="."
fi
real_tool="${{this_dir}}/../tools/{real_tool}"
if [[ "${{OSTYPE:-}}" == linux* ]]; then
  toolchain_root="${{this_dir}}/.."
  export LD_LIBRARY_PATH="${{toolchain_root}}/runtime_libs:${{toolchain_root}}/llvm/libexec:${{toolchain_root}}/llvm/lib:${{LD_LIBRARY_PATH:-}}"
  exec "${{real_tool}}" "$@"
fi
if [[ "${{MONGO_WINDOWS_CROSS_ACTION_WRAPPER:-1}}" == "0" ]]; then
  exec "${{real_tool}}" "$@"
fi
wrapper="${{MONGO_WINDOWS_CROSS_ACTION_WRAPPER_SCRIPT:-bazel/toolchains/cc/mongo_windows_cross/windows_cross_action_wrapper.py}}"
python="${{MONGO_WINDOWS_CROSS_ACTION_PYTHON:-python3}}"
exec "${{python}}" "${{wrapper}}" "${{real_tool}}" "$@"
"""

# Bazel uses the same host-wrapper toolchain for local Windows actions and for
# actions sent to Linux RBE workers. A POSIX shell script cannot be started by
# Windows CreateProcessW, while a Windows-only batch file cannot be executed by
# the Linux worker. The first line is a POSIX command when run through execvp's
# ENOEXEC shell fallback and a batch-file label on Windows, so the same file
# works in both environments.
_WINDOWS_WRAPPER_TEMPLATE = """:; this_path="$0"; this_dir="${{this_path%/*}}"; if [ "$this_dir" = "$this_path" ]; then this_dir="."; fi; toolchain_root="$this_dir/.."; export LD_LIBRARY_PATH="$toolchain_root/runtime_libs:$toolchain_root/llvm/libexec:$toolchain_root/llvm/lib:${{LD_LIBRARY_PATH:-}}"; exec "$toolchain_root/tools/{real_tool}" "$@"
@echo off
setlocal
set "this_dir=%~dp0"
set "real_tool=%this_dir%..\\tools\\{real_tool}"
set "wrapper=%MONGO_WINDOWS_CROSS_ACTION_WRAPPER_SCRIPT%"
if not defined wrapper set "wrapper=bazel\\toolchains\\cc\\mongo_windows_cross\\windows_cross_action_wrapper.py"
set "python=%MONGO_WINDOWS_CROSS_ACTION_PYTHON%"
rem The action PATH intentionally excludes Windows system directories, so do not use where.exe.
if not defined python set "python=python.exe"
:run_wrapper
"%python%" "%wrapper%" "%real_tool%" %*
exit /b %ERRORLEVEL%
"""

_SYSROOT_DIRS = [
    "msvc/include",
    "msvc/lib/x64",
    "msvc/atlmfc/include",
    "msvc/atlmfc/lib/x64",
    "winsdk/include/ucrt",
    "winsdk/include/shared",
    "winsdk/include/um",
    "winsdk/include/winrt",
    "winsdk/include/cppwinrt",
    "winsdk/lib/ucrt/x64",
    "winsdk/lib/um/x64",
]

_SYSROOT_INCLUDE_DIRS = [
    "msvc/include",
    "msvc/atlmfc/include",
    "winsdk/include/ucrt",
    "winsdk/include/shared",
    "winsdk/include/um",
    "winsdk/include/winrt",
    "winsdk/include/cppwinrt",
]

def get_supported_windows_cross_archs():
    return _CROSS_COMPILE_ARCHS

_EXEC_ARCH_ENV = "MONGO_WINDOWS_CROSS_EXEC_ARCH"
_EXEC_ARCH_ENVS = [_EXEC_ARCH_ENV, "MONGO_WINDOWS_CROSS_RBE_POOL"]

def _execution_arch(repository_ctx):
    """Validates x86_64 execution for the LLVM archive and toolchain constraints."""
    env = repository_ctx.os.environ
    value = env.get(_EXEC_ARCH_ENV, "x86_64").lower()
    if value not in ["x86_64", "amd64"]:
        fail("Windows cross toolchains require " + _EXEC_ARCH_ENV + "=x86_64")
    pool = env.get("MONGO_WINDOWS_CROSS_RBE_POOL", "").lower()
    if pool in ["arm64", "aarch64", "default"]:
        fail("Windows cross RBE pool " + pool + " is incompatible with x86_64 execution")
    return "x86_64"

def _windows_cross_toolchain_registration_impl(repository_ctx):
    os_name = repository_ctx.os.name.lower()
    if "linux" not in os_name and "windows" not in os_name:
        repository_ctx.file(
            "BUILD.bazel",
            "# Windows cross-compilation toolchain is only available from Linux or Windows\n",
        )
        return

    exec_arch = _execution_arch(repository_ctx)
    entries = [
        'package(default_visibility = ["//visibility:public"])\n',
    ]
    for arch, cpu in _CROSS_COMPILE_ARCHS.items():
        # The repository runs on the Bazel client, not the execution worker.
        # Windows clients also need this toolchain for their Linux RBE actions.
        entries.append("""
toolchain(
    name = "mongo_windows_cross_{arch}_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{exec_arch}",
    ],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
        "@//bazel/platforms:use_mongo_windows_cross_toolchain",
    ],
    toolchain = "@mongo_windows_cross_toolchain_files//:cc-compiler-{arch}",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(arch = arch, cpu = cpu, exec_arch = exec_arch))
        if "windows" in os_name:
            entries.append("""
toolchain(
    name = "mongo_windows_cross_{arch}_windows_host_toolchain",
    exec_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
        "@//bazel/platforms:use_mongo_windows_cross_toolchain",
    ],
    toolchain = "@mongo_windows_cross_toolchain_files//:cc-compiler-{arch}-host-wrapper",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(arch = arch, cpu = cpu))

    repository_ctx.file("BUILD.bazel", "\n".join(entries))

mongo_windows_cross_toolchain_config = repository_rule(
    implementation = _windows_cross_toolchain_registration_impl,
    environ = _EXEC_ARCH_ENVS,
    configure = True,
)

def _archive_value(repository_ctx, env_name, default_value):
    return repository_ctx.os.environ.get(env_name, default_value)

def _default_llvm_archive(repository_ctx):
    _execution_arch(repository_ctx)
    return _LLVM_X64_URL, _LLVM_X64_SHA256, _LLVM_X64_STRIP_PREFIX

def _copy_tree(repository_ctx, source, output):
    if "windows" in repository_ctx.os.name.lower():
        result = repository_ctx.execute(
            [
                "robocopy",
                source,
                str(repository_ctx.path(output)),
                "/MIR",
                "/NFL",
                "/NDL",
                "/NJH",
                "/NJS",
                "/NC",
                "/NS",
                "/NP",
            ],
            quiet = True,
        )
        if result.return_code >= 8:
            fail("Failed to copy " + source + " to " + output + ":\n" + result.stderr)
        return

    repository_ctx.execute(["cp", "-a", source + "/.", str(repository_ctx.path(output))])

def _download_archive(
        repository_ctx,
        name,
        output,
        url_env,
        sha_env,
        strip_prefix_env,
        default_url,
        default_sha,
        default_strip_prefix):
    local_path = repository_ctx.os.environ.get(name + "_PATH", "")
    if local_path:
        if "windows" in repository_ctx.os.name.lower() and name == "MONGO_WINDOWS_CROSS_SYSROOT":
            _copy_tree(repository_ctx, local_path, output)
        else:
            repository_ctx.symlink(local_path, output)
        return str(repository_ctx.path(output))

    url = _archive_value(repository_ctx, url_env, default_url)
    sha256 = _archive_value(repository_ctx, sha_env, default_sha)
    strip_prefix = _archive_value(repository_ctx, strip_prefix_env, default_strip_prefix)
    if not url or not sha256:
        fail("""Windows cross-compilation requires hermetic toolchain archives.
Set {url_env} and {sha_env}, or set {name}_PATH to a local unpacked tree.
On Windows hosts, the hermetic_container wrapper can generate MONGO_WINDOWS_CROSS_SYSROOT_PATH
from the pinned local MSVC and Windows SDK installation.
The sysroot tree must use the layout documented in bazel/docs/windows_hermetic_container_cross.md.""".format(
            name = name,
            url_env = url_env,
            sha_env = sha_env,
        ))

    repository_ctx.report_progress("Downloading " + name + " for Windows cross-compilation")
    if strip_prefix:
        repository_ctx.download_and_extract(
            url = url,
            sha256 = sha256,
            output = output,
            stripPrefix = strip_prefix,
        )
    else:
        repository_ctx.download_and_extract(
            url = url,
            sha256 = sha256,
            output = output,
        )
    return str(repository_ctx.path(output))

def _tool(repository_ctx, root, name, output_name = None):
    output_name = output_name or name
    path = root + "/bin/" + name
    if repository_ctx.path(path).exists:
        repository_ctx.symlink(path, "tools/" + output_name)
        return

    for fallback_name in _LLVM_TOOL_FALLBACKS.get(name, []):
        fallback_path = root + "/bin/" + fallback_name
        if repository_ctx.path(fallback_path).exists:
            repository_ctx.symlink(fallback_path, "tools/" + output_name)
            return

    if not repository_ctx.path(path).exists:
        fail("Required LLVM tool not found: " + path)

def _validate_sysroot(repository_ctx, sysroot_path):
    missing = []
    for relative in _SYSROOT_DIRS:
        path = sysroot_path + "/" + relative
        if not repository_ctx.path(path).exists:
            missing.append(relative)
    if missing:
        fail(
            "Windows cross sysroot is missing required directories:\n  " +
            "\n  ".join(missing) +
            "\nExpected layout is documented in bazel/docs/windows_hermetic_container_cross.md.",
        )

def _clang_version(repository_ctx, llvm_path):
    version = repository_ctx.os.environ.get("MONGO_WINDOWS_CROSS_LLVM_VERSION", "")
    if version:
        return version.split(".", 1)[0]

    clang = llvm_path + "/bin/clang"
    clang_lib = repository_ctx.path(llvm_path + "/lib/clang")
    if clang_lib.exists:
        versions = [
            entry.basename
            for entry in clang_lib.readdir()
            if entry.basename and entry.basename[0].isdigit()
        ]
        if len(versions) == 1:
            return versions[0].split(".", 1)[0]

    if "windows" not in repository_ctx.os.name.lower():
        result = repository_ctx.execute([clang, "--version"])
        if result.return_code != 0:
            fail("Failed to run " + clang + " --version:\n" + result.stderr)
        for line in result.stdout.split("\n"):
            if "clang version " in line:
                version = line.split("clang version ", 1)[1].split(" ", 1)[0]
                major = version.split(".", 1)[0]
                return major
        fail("Could not determine clang version from:\n" + result.stdout)

    fail("Could not determine clang version. Set MONGO_WINDOWS_CROSS_LLVM_VERSION.")

def _symlink_toolchain_files(repository_ctx, llvm_path):
    for tool in _LLVM_TOOLS:
        _tool(
            repository_ctx,
            llvm_path,
            tool,
            output_name = "clang-real" if tool == "clang" else None,
        )
    for tool in _OPTIONAL_LLVM_TOOLS:
        path = llvm_path + "/bin/" + tool
        if repository_ctx.path(path).exists:
            repository_ctx.symlink(path, "tools/" + tool)

    clang_major = _clang_version(repository_ctx, llvm_path)
    clang_include = llvm_path + "/lib/clang/" + clang_major + "/include"
    if not repository_ctx.path(clang_include).exists:
        fail("Clang builtin headers not found: " + clang_include)

    # -resource-dir requires an include/ child. Stage it explicitly: the original
    # LLVM tree is not part of compiler_files and is absent on RBE workers.
    # Keep a single copy: Clang's MSVC shim headers use include_next, which must
    # reach the SDK rather than another copy of the same guarded shim.
    repository_ctx.symlink(clang_include, "resource_dir/include")

    clang_windows_lib = llvm_path + "/lib/clang/" + clang_major + "/lib/windows"
    if repository_ctx.path(clang_windows_lib).exists:
        repository_ctx.symlink(clang_windows_lib, "lib/clang/windows")
    return clang_major

def _write_host_wrapper_scripts(repository_ctx):
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
        repository_ctx.file(
            "wrappers/" + wrapper_name + ".cmd",
            _WINDOWS_WRAPPER_TEMPLATE.format(real_tool = wrapper_name),
            executable = True,
        )

def _write_runtime_libs(repository_ctx, default_llvm_url, default_llvm_sha256):
    if "windows" not in repository_ctx.os.name.lower():
        return

    # The pinned minimal x64 archive uses only the execution image's system libraries.
    # Local trees and full archives still need their bundled shared libraries staged.
    llvm_url = _archive_value(repository_ctx, "MONGO_WINDOWS_CROSS_LLVM_URL", default_llvm_url)
    llvm_sha256 = _archive_value(repository_ctx, "MONGO_WINDOWS_CROSS_LLVM_SHA256", default_llvm_sha256)
    if (not repository_ctx.os.environ.get("MONGO_WINDOWS_CROSS_LLVM_PATH", "") and
        llvm_url == _LLVM_X64_URL and llvm_sha256 == _LLVM_X64_SHA256):
        return

    repository_ctx.file(
        "copy_runtime_libs.ps1",
        """
$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
$output = Join-Path $repo 'runtime_libs'
New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($relativeDir in @('llvm/libexec', 'llvm/lib')) {
    $dir = Join-Path $repo $relativeDir
    if (!(Test-Path $dir)) {
        continue
    }
    Get-ChildItem -Path $dir -File -Filter 'lib*.so*' | ForEach-Object {
        $dest = Join-Path $output $_.Name
        if ($_.Length -gt 0) {
            Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
            return
        }
        $target = Get-ChildItem -Path $dir -File -Filter ($_.Name + '.*') |
            Where-Object { $_.Length -gt 0 } |
            Sort-Object Name |
            Select-Object -First 1
        if ($target) {
            Copy-Item -LiteralPath $target.FullName -Destination $dest -Force
        }
    }
}
""",
    )
    result = repository_ctx.execute(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(repository_ctx.path("copy_runtime_libs.ps1")),
        ],
        quiet = True,
    )
    if result.return_code != 0:
        fail("Failed to copy Windows cross runtime libs:\n" + result.stderr)

def _write_case_insensitive_lib_aliases(repository_ctx):
    python = repository_ctx.which("python3")
    if python == None:
        python = repository_ctx.which("python")
    if python == None:
        fail("Could not find Python 3 to generate Windows cross library aliases")

    result = repository_ctx.execute([
        str(python),
        str(repository_ctx.path(Label(
            "@//bazel/toolchains/cc/mongo_windows_cross:windows_cross_lib_aliases.py",
        ))),
        "--repo",
        str(repository_ctx.path(".")),
    ], quiet = True)

    if result.return_code != 0:
        fail("Failed to copy Windows cross import library aliases:\n" + result.stderr)

def _write_case_insensitive_vfs_overlay(repository_ctx, execroot_prefix):
    python = repository_ctx.which("python3")
    if python == None:
        python = repository_ctx.which("python")
    if python == None:
        fail("Could not find Python 3 to generate the Windows cross VFS overlay")

    result = repository_ctx.execute(
        [
            str(python),
            str(repository_ctx.path(Label(
                "@//bazel/toolchains/cc/mongo_windows_cross:write_case_insensitive_vfs.py",
            ))),
            "--repo",
            str(repository_ctx.path(".")),
            "--execroot-prefix",
            execroot_prefix,
            "--output",
            str(repository_ctx.path("case_insensitive_vfs.yaml")),
        ] + _SYSROOT_INCLUDE_DIRS,
        quiet = True,
    )
    if result.return_code != 0:
        fail("Failed to generate Windows cross VFS overlay:\n" + result.stderr)

def _write_rc_clang_wrapper(repository_ctx):
    repository_ctx.file(
        "tools/clang",
        _RC_CLANG_WRAPPER_TEMPLATE,
        executable = True,
    )

def _windows_cross_toolchain_files_impl(repository_ctx):
    os_name = repository_ctx.os.name.lower()
    if "linux" not in os_name and "windows" not in os_name:
        repository_ctx.file(
            "BUILD.bazel",
            "# Windows cross-compilation toolchain is only available from Linux or Windows\n",
        )
        return

    llvm_url, llvm_sha256, llvm_strip_prefix = _default_llvm_archive(repository_ctx)
    llvm_path = _download_archive(
        repository_ctx,
        "MONGO_WINDOWS_CROSS_LLVM",
        "llvm",
        "MONGO_WINDOWS_CROSS_LLVM_URL",
        "MONGO_WINDOWS_CROSS_LLVM_SHA256",
        "MONGO_WINDOWS_CROSS_LLVM_STRIP_PREFIX",
        llvm_url,
        llvm_sha256,
        llvm_strip_prefix,
    )
    sysroot_path = _download_archive(
        repository_ctx,
        "MONGO_WINDOWS_CROSS_SYSROOT",
        "sysroot",
        "MONGO_WINDOWS_CROSS_SYSROOT_URL",
        "MONGO_WINDOWS_CROSS_SYSROOT_SHA256",
        "MONGO_WINDOWS_CROSS_SYSROOT_STRIP_PREFIX",
        _SYSROOT_URL,
        _SYSROOT_SHA256,
        _SYSROOT_STRIP_PREFIX,
    )
    _validate_sysroot(repository_ctx, sysroot_path)
    _symlink_toolchain_files(repository_ctx, llvm_path)
    _write_host_wrapper_scripts(repository_ctx)
    _write_runtime_libs(repository_ctx, llvm_url, llvm_sha256)
    _write_case_insensitive_lib_aliases(repository_ctx)

    prefix = "external/" + repository_ctx.name
    _write_case_insensitive_vfs_overlay(repository_ctx, prefix)
    _write_rc_clang_wrapper(repository_ctx)
    include_dirs = [
        prefix + "/resource_dir/include",
        prefix + "/sysroot/msvc/include",
        prefix + "/sysroot/msvc/atlmfc/include",
        prefix + "/sysroot/winsdk/include/ucrt",
        prefix + "/sysroot/winsdk/include/shared",
        prefix + "/sysroot/winsdk/include/um",
        prefix + "/sysroot/winsdk/include/winrt",
        prefix + "/sysroot/winsdk/include/cppwinrt",
    ]
    lib_dirs = [
        prefix + "/lib/clang/windows",
        prefix + "/lib_aliases/original_base_lower_extension/msvc/lib/x64",
        prefix + "/lib_aliases/original_base_lower_extension/msvc/atlmfc/lib/x64",
        prefix + "/lib_aliases/original_base_lower_extension/winsdk/lib/ucrt/x64",
        prefix + "/lib_aliases/original_base_lower_extension/winsdk/lib/um/x64",
        prefix + "/lib_aliases/first_upper_lower_rest/msvc/lib/x64",
        prefix + "/lib_aliases/first_upper_lower_rest/msvc/atlmfc/lib/x64",
        prefix + "/lib_aliases/first_upper_lower_rest/winsdk/lib/ucrt/x64",
        prefix + "/lib_aliases/first_upper_lower_rest/winsdk/lib/um/x64",
        prefix + "/lib_aliases/upper_base_lower_extension/msvc/lib/x64",
        prefix + "/lib_aliases/upper_base_lower_extension/msvc/atlmfc/lib/x64",
        prefix + "/lib_aliases/upper_base_lower_extension/winsdk/lib/ucrt/x64",
        prefix + "/lib_aliases/upper_base_lower_extension/winsdk/lib/um/x64",
        prefix + "/lib_aliases/lower/msvc/lib/x64",
        prefix + "/lib_aliases/lower/msvc/atlmfc/lib/x64",
        prefix + "/lib_aliases/lower/winsdk/lib/ucrt/x64",
        prefix + "/lib_aliases/lower/winsdk/lib/um/x64",
        prefix + "/sysroot/msvc/lib/x64",
        prefix + "/sysroot/msvc/atlmfc/lib/x64",
        prefix + "/sysroot/winsdk/lib/ucrt/x64",
        prefix + "/sysroot/winsdk/lib/um/x64",
    ]
    lib_path_flags = ["/LIBPATH:" + path for path in lib_dirs]

    host_python_files = []
    host_msvc_env_path = prefix + "/wrappers;" + prefix + "/tools"
    if "windows" in os_name:
        host_python = repository_ctx.path(Label("@py_host//:dist/python.exe"))
        host_python_repo = host_python.dirname.dirname.basename
        host_python_files = ["@py_host//:files"]
        host_msvc_env_path += ";external/" + host_python_repo + "/dist"

    repository_ctx.template(
        "BUILD.bazel",
        Label("@//bazel/toolchains/cc/mongo_windows_cross:BUILD.tmpl"),
        {
            "%{execroot_prefix}": prefix,
            "%{include_dirs}": repr(include_dirs),
            "%{lib_dirs}": repr(lib_dirs),
            "%{lib_env}": ";".join(lib_dirs),
            "%{lib_path_flags}": repr(lib_path_flags),
            "%{host_msvc_env_path}": host_msvc_env_path,
            "%{host_python_files}": repr(host_python_files),
            "%{clang_resource_dir}": prefix + "/resource_dir",
        },
    )

mongo_windows_cross_toolchain_files_config = repository_rule(
    environ = _EXEC_ARCH_ENVS + [
        "BAZEL_VS",
        "BAZEL_VC",
        "BAZEL_VC_FULL_VERSION",
        "BAZEL_WINSDK_FULL_VERSION",
        "MONGO_VC_REDIST_FULL_VERSION",
        "MONGO_WINDOWS_CROSS_LLVM_PATH",
        "MONGO_WINDOWS_CROSS_LLVM_URL",
        "MONGO_WINDOWS_CROSS_LLVM_SHA256",
        "MONGO_WINDOWS_CROSS_LLVM_STRIP_PREFIX",
        "MONGO_WINDOWS_CROSS_LLVM_VERSION",
        "MONGO_WINDOWS_CROSS_SYSROOT_PATH",
        "MONGO_WINDOWS_CROSS_SYSROOT_URL",
        "MONGO_WINDOWS_CROSS_SYSROOT_SHA256",
        "MONGO_WINDOWS_CROSS_SYSROOT_STRIP_PREFIX",
        "MONGO_WINDOWS_CROSS_WINSDK_ROOT",
        "BAZEL_WINSDK_ROOT",
        "WINDOWSSDKDIR",
    ],
    implementation = _windows_cross_toolchain_files_impl,
    configure = True,
)

def setup_mongo_windows_cross_toolchain():
    mongo_windows_cross_toolchain_config(
        name = "mongo_windows_cross_toolchain",
    )
    mongo_windows_cross_toolchain_files_config(
        name = "mongo_windows_cross_toolchain_files",
    )

setup_mongo_windows_cross_toolchain_extension = module_extension(
    implementation = lambda ctx: setup_mongo_windows_cross_toolchain(),
)
