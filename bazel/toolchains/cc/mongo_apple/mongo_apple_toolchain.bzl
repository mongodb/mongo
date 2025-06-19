load("@build_bazel_apple_support//configs:platforms.bzl", "APPLE_PLATFORMS_CONSTRAINTS")

def _get_llvm_info(repository_ctx, build_file):
    llvm_version = repository_ctx.os.environ.get("LLVM_VERSION") or ""

    if llvm_version == "":
        error_message = """
The Apple LLVM Clang toolchain has not been defined. Please make sure
that LLVM_VERSION has been defined in //.bazelrc.local or //.bazelrc file."""
        return False, "", "", error_message

    brew_command = [
        "/bin/bash",
        "-c",
        "brew --prefix llvm@{}".format(llvm_version),
    ]
    result = repository_ctx.execute(brew_command)
    if result.return_code != 0:
        error_message = """
Unable to find the prefix LLVM path using brew command: {}. Please make
sure that you have installed the LLVM toolchain using Homebrew:
    `brew install llvm@{} lld@{}`.
or update the LLVM_VERSION in the //.bazelrc file or //.bazelrc.local.""".format(" ".join(brew_command), llvm_version, llvm_version)
        return False, "", "", error_message
    llvm_path = result.stdout.strip()

    # Find the real path to the LLVM installation as we need to include the LLVM
    # lib and headers directories as part of built-in directories.
    command = [
        "/bin/bash",
        "-c",
        "readlink -f {}".format(llvm_path),
    ]
    result = repository_ctx.execute(command)
    if result.return_code != 0:
        return False, "", "", """Failed to find the true LLVM path using command: {}. Please make
sure that you have installed the LLVM toolchain using Homebrew:
    `brew install llvm@{} lld@{}""".format(" ".join(command), llvm_version, llvm_version)
    llvm_path = result.stdout.strip()

    return True, llvm_path, llvm_version, ""

def _get_lld_info(repository_ctx, llvm_version):
    error_message = """
Unable to find the lld path. Please make sure that you have installed the lld using Homebrew: 
    `brew install lld@{}`.""".format(llvm_version)

    brew_command = [
        "/bin/bash",
        "-c",
        "brew --prefix lld@{}".format(llvm_version),
    ]
    result = repository_ctx.execute(brew_command)
    if result.return_code != 0:
        return False, "", error_message
    lld_path = result.stdout.strip()

    command = [
        "/bin/bash",
        "-c",
        "readlink -f {}".format(lld_path),
    ]
    result = repository_ctx.execute(command)
    if result.return_code != 0:
        return False, "", error_message
    lld_path = result.stdout.strip()

    return True, lld_path, ""

def _get_llvm_clang_include_dirs(repository_ctx, llvm_path):
    include_dirs = [
        "/Applications/",
        "/Library",
    ]

    user = repository_ctx.os.environ.get("USER")
    if user:
        include_dirs.extend([
            "/Users/{}/Applications/".format(user),
            "/Users/{}/Library/".format(user),
        ])

    for include_dir in ["include", "lib"]:
        include_dirs.append(llvm_path + "/" + include_dir)

    ret_include_dirs = []
    for path in include_dirs:
        ret_include_dirs.append(("            \"%s\"," % path))

    return ret_include_dirs

def _configure_oss_clang_toolchain(repository_ctx):
    build_file = "BUILD.bazel"

    success, llvm_path, llvm_version, error = _get_llvm_info(repository_ctx, build_file)
    if not success:
        return False, error

    success, lld_path, error = _get_lld_info(repository_ctx, llvm_version)
    if not success:
        return False, error

    include_dirs = _get_llvm_clang_include_dirs(repository_ctx, llvm_path)

    repository_ctx.report_progress("Generating Apple OSS LLVM Clang Toolchain build file")
    build_template = Label("@//bazel/toolchains/cc/mongo_apple:BUILD.tmpl")
    repository_ctx.template(
        build_file,
        build_template,
        {
            "%{llvm_path}": llvm_path,
            "%{lld_path}": lld_path,
            "%{cxx_builtin_include_directories}": "\n".join(include_dirs),
        },
    )

    return True, ""

def _apple_llvm_clang_cc_autoconf_impl(repository_ctx):
    """Configures the Apple LLVM Clang toolchain."""
    if repository_ctx.os.name.startswith("mac os"):
        # No failure is shown to the user as the toolchain is still being worked on it.
        success, error_msg = _configure_oss_clang_toolchain(repository_ctx)
        if not success:
            fail(error_msg)
    else:
        repository_ctx.file("BUILD", "# Apple OSS LLVM Clang autoconfiguration was disabled because you're not on macOS")

mongo_apple_brew_llvm_toolchain_config = repository_rule(
    environ = [
        "LLVM_PATH",  # Force re-compute if the user changed the location of the LLVM toolchain
        "LLVM_VERSION",  # Force re-compute if the user changed the version of the LLVM toolchain
    ],
    implementation = _apple_llvm_clang_cc_autoconf_impl,
    configure = True,
    local = True,
)

_ARCH_MAP = {
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
    "ppc64le": "@platforms//cpu:ppc",
    "s390x": "@platforms//cpu:s390x",
}

def get_supported_apple_archs():
    _APPLE_ARCHS = APPLE_PLATFORMS_CONSTRAINTS.keys()
    supported_archs = {}
    for arch in APPLE_PLATFORMS_CONSTRAINTS.keys():
        if arch.startswith("darwin_"):
            cpu = arch.replace("darwin_", "")
            if cpu in ["x86_64", "arm64"]:
                supported_archs[arch] = cpu
    return supported_archs

def setup_mongo_apple_toolchain():
    mongo_apple_brew_llvm_toolchain_config(
        name = "mongo_apple_toolchain",
    )

setup_mongo_apple_toolchain_extension = module_extension(
    implementation = lambda ctx: setup_mongo_apple_toolchain(),
)
