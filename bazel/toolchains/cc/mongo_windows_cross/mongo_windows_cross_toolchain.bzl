"""Repository rules for MongoDB's Windows cross-compilation toolchain.

The toolchain executes on Linux or through host wrappers and targets Windows
x86_64. It is intentionally separate from the native Windows/MSVC toolchain so
Windows developer builds keep their current host-local Visual Studio behavior.
"""

_LLVM_URL = ""
_LLVM_SHA256 = ""
_LLVM_STRIP_PREFIX = ""
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

_LLVM_TOOL_FALLBACKS = {
    "clang": ["clang-21", "clang-20", "clang-19", "clang-18"],
    "clang++": ["clang-21", "clang-20", "clang-19", "clang-18"],
    "clang-cl": ["clang-21", "clang-20", "clang-19", "clang-18"],
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

def _windows_cross_toolchain_registration_impl(repository_ctx):
    os_name = repository_ctx.os.name.lower()
    if "linux" not in os_name and "windows" not in os_name:
        repository_ctx.file(
            "BUILD.bazel",
            "# Windows cross-compilation toolchain is only available from Linux or Windows\n",
        )
        return

    entries = [
        'package(default_visibility = ["//visibility:public"])\n',
    ]
    for arch, cpu in _CROSS_COMPILE_ARCHS.items():
        if "linux" in os_name:
            entries.append("""
toolchain(
    name = "mongo_windows_cross_{arch}_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
        "@//bazel/platforms:use_mongo_windows_cross_toolchain",
    ],
    toolchain = "@mongo_windows_cross_toolchain_files//:cc-compiler-{arch}",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(arch = arch, cpu = cpu))
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
    configure = True,
)

def _archive_value(repository_ctx, env_name, default_value):
    return repository_ctx.os.environ.get(env_name, default_value)

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

def _tool(repository_ctx, root, name):
    path = root + "/bin/" + name
    if repository_ctx.path(path).exists:
        repository_ctx.symlink(path, "tools/" + name)
        return

    for fallback_name in _LLVM_TOOL_FALLBACKS.get(name, []):
        fallback_path = root + "/bin/" + fallback_name
        if repository_ctx.path(fallback_path).exists:
            repository_ctx.symlink(fallback_path, "tools/" + name)
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
        _tool(repository_ctx, llvm_path, tool)

    clang_major = _clang_version(repository_ctx, llvm_path)
    clang_include = llvm_path + "/lib/clang/" + clang_major + "/include"
    if not repository_ctx.path(clang_include).exists:
        fail("Clang builtin headers not found: " + clang_include)
    repository_ctx.symlink(clang_include, "include/clang")

    clang_windows_lib = llvm_path + "/lib/clang/" + clang_major + "/lib/windows"
    if repository_ctx.path(clang_windows_lib).exists:
        repository_ctx.symlink(clang_windows_lib, "lib/clang/windows")

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

def _write_runtime_libs(repository_ctx):
    if "windows" not in repository_ctx.os.name.lower():
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
    lib_dirs = [
        "msvc/lib/x64",
        "msvc/atlmfc/lib/x64",
        "winsdk/lib/ucrt/x64",
        "winsdk/lib/um/x64",
    ]

    if "windows" in repository_ctx.os.name.lower():
        repository_ctx.file(
            "copy_lib_aliases.ps1",
            """
$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
$libDirs = @(%s)
Remove-Item -LiteralPath (Join-Path $repo 'lib_aliases') -Recurse -Force -ErrorAction SilentlyContinue
function Copy-LibAlias($sourceFile, $relativeDir, $variantDir, $aliasName) {
    if ($aliasName -ceq $sourceFile.Name) {
        return
    }
    $outputDir = Join-Path $repo ("lib_aliases/" + $variantDir + "/" + $relativeDir)
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    Copy-Item -LiteralPath $sourceFile.FullName -Destination (Join-Path $outputDir $aliasName) -Force
}
foreach ($relativeDir in $libDirs) {
    $sourceDir = Join-Path $repo ("sysroot/" + $relativeDir)
    if (!(Test-Path $sourceDir)) {
        continue
    }
    Get-ChildItem -Path $sourceDir -File -Filter '*.lib' | ForEach-Object {
        $sourceFile = $_
        $lowerName = $_.Name.ToLowerInvariant()
        $firstLetterUpperLowerRestName = $_.Name.Substring(0, 1).ToUpperInvariant() + $lowerName.Substring(1)
        $originalBaseLowerExtensionName = [System.IO.Path]::GetFileNameWithoutExtension($_.Name) + ".lib"
        Copy-LibAlias $sourceFile $relativeDir "lower" $lowerName
        Copy-LibAlias $sourceFile $relativeDir "first_upper_lower_rest" $firstLetterUpperLowerRestName
        Copy-LibAlias $sourceFile $relativeDir "original_base_lower_extension" $originalBaseLowerExtensionName
    }
}
""" % ", ".join(['"' + lib_dir + '"' for lib_dir in lib_dirs]),
        )
        result = repository_ctx.execute(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(repository_ctx.path("copy_lib_aliases.ps1")),
            ],
            quiet = True,
        )
    else:
        repository_ctx.file(
            "copy_lib_aliases.sh",
            """
set -euo pipefail
repo="$PWD"
rm -rf "$repo/lib_aliases"
for relative_dir in %s; do
  source_dir="$repo/sysroot/$relative_dir"
  if [[ ! -d "$source_dir" ]]; then
    continue
  fi
  while IFS= read -r -d '' file; do
    name="${file##*/}"
    lower_name="$(printf '%%s' "$name" | tr '[:upper:]' '[:lower:]')"
    first_letter="$(printf '%%s' "${lower_name:0:1}" | tr '[:lower:]' '[:upper:]')"
    first_letter_upper_lower_rest_name="${first_letter}${lower_name:1}"
    original_base_lower_extension_name="${name%.*}.lib"
    lower_output_dir="$repo/lib_aliases/lower/$relative_dir"
    first_upper_output_dir="$repo/lib_aliases/first_upper_lower_rest/$relative_dir"
    original_base_lower_extension_output_dir="$repo/lib_aliases/original_base_lower_extension/$relative_dir"
    mkdir -p "$lower_output_dir" "$first_upper_output_dir" "$original_base_lower_extension_output_dir"
    if [[ "$lower_name" != "$name" && ! -e "$lower_output_dir/$lower_name" ]]; then
      cp -f "$file" "$lower_output_dir/$lower_name"
    fi
    if [[ "$first_letter_upper_lower_rest_name" != "$name" && ! -e "$first_upper_output_dir/$first_letter_upper_lower_rest_name" ]]; then
      cp -f "$file" "$first_upper_output_dir/$first_letter_upper_lower_rest_name"
    fi
    if [[ "$original_base_lower_extension_name" != "$name" && ! -e "$original_base_lower_extension_output_dir/$original_base_lower_extension_name" ]]; then
      cp -f "$file" "$original_base_lower_extension_output_dir/$original_base_lower_extension_name"
    fi
  done < <(find "$source_dir" -maxdepth 1 -type f -iname '*.lib' -print0)
done
""" % " ".join(['"' + lib_dir + '"' for lib_dir in lib_dirs]),
            executable = True,
        )
        result = repository_ctx.execute(
            ["bash", str(repository_ctx.path("copy_lib_aliases.sh"))],
            quiet = True,
        )

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

def _windows_cross_toolchain_files_impl(repository_ctx):
    os_name = repository_ctx.os.name.lower()
    if "linux" not in os_name and "windows" not in os_name:
        repository_ctx.file(
            "BUILD.bazel",
            "# Windows cross-compilation toolchain is only available from Linux or Windows\n",
        )
        return

    llvm_path = _download_archive(
        repository_ctx,
        "MONGO_WINDOWS_CROSS_LLVM",
        "llvm",
        "MONGO_WINDOWS_CROSS_LLVM_URL",
        "MONGO_WINDOWS_CROSS_LLVM_SHA256",
        "MONGO_WINDOWS_CROSS_LLVM_STRIP_PREFIX",
        _LLVM_URL,
        _LLVM_SHA256,
        _LLVM_STRIP_PREFIX,
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
    _write_runtime_libs(repository_ctx)
    _write_case_insensitive_lib_aliases(repository_ctx)

    prefix = "external/" + repository_ctx.name
    _write_case_insensitive_vfs_overlay(repository_ctx, prefix)
    include_dirs = [
        prefix + "/include/clang",
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

    repository_ctx.template(
        "BUILD.bazel",
        Label("@//bazel/toolchains/cc/mongo_windows_cross:BUILD.tmpl"),
        {
            "%{execroot_prefix}": prefix,
            "%{include_dirs}": repr(include_dirs),
            "%{lib_dirs}": repr(lib_dirs),
            "%{lib_env}": ";".join(lib_dirs),
            "%{lib_path_flags}": repr(lib_path_flags),
        },
    )

mongo_windows_cross_toolchain_files_config = repository_rule(
    environ = [
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
