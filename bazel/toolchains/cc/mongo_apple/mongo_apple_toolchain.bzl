load("@build_bazel_apple_support//configs:platforms.bzl", "APPLE_PLATFORMS_CONSTRAINTS")

# Pre-built LLVM toolchains for native macOS builds. These are the official
# LLVM release archives from https://github.com/llvm/llvm-project/releases,
# stripped down with package_llvm.sh and mirrored to S3.
_S3_BUCKET = "https://mdb-build-public.s3.us-east-1.amazonaws.com"
_S3_PREFIX = "toolchains"
_LLVM_VERSION = "19.1.7"

# Keyed by repository_ctx.os.arch of the macOS host.
_LLVM_ARCHIVES = {
    "aarch64": struct(
        url = _S3_BUCKET + "/" + _S3_PREFIX + "/llvm-" + _LLVM_VERSION + "-macos-arm64.tar.xz",
        sha256 = "5cf56b2f90c5ec48196c653e9abb0589c894c86166caa5d7553cad040ee83f70",
        strip_prefix = "llvm-" + _LLVM_VERSION + "-macos-arm64",
    ),
    "x86_64": struct(
        url = _S3_BUCKET + "/" + _S3_PREFIX + "/llvm-" + _LLVM_VERSION + "-macos-x86_64.tar.xz",
        sha256 = "77f8d6727c045342fe0b2381900c5b09cf644de1c273b483173612266fbe655e",
        strip_prefix = "llvm-" + _LLVM_VERSION + "-macos-x86_64",
    ),
}
_LLVM_ARCHIVES["arm64"] = _LLVM_ARCHIVES["aarch64"]
_LLVM_ARCHIVES["amd64"] = _LLVM_ARCHIVES["x86_64"]

# Directory within this repository that the LLVM archive is extracted to.
_LLVM_DIR = "llvm"

def _download_llvm(repository_ctx):
    """Downloads the LLVM toolchain for the host architecture into _LLVM_DIR."""
    arch = repository_ctx.os.arch
    archive = _LLVM_ARCHIVES.get(arch)
    if not archive:
        return False, "No pre-built macOS LLVM toolchain is available for host architecture '{}'. Set LLVM_PATH to a local LLVM {} installation.".format(arch, _LLVM_VERSION)

    repository_ctx.report_progress("Downloading LLVM {} for macOS {}".format(_LLVM_VERSION, arch))
    repository_ctx.download_and_extract(
        url = archive.url,
        sha256 = archive.sha256,
        output = _LLVM_DIR,
        stripPrefix = archive.strip_prefix,
    )
    return True, ""

def _get_llvm_info(repository_ctx):
    """Returns (success, tool_path, abs_path, error).

    tool_path is used for the toolchain's tool_paths and is relative to this
    repository when the toolchain was downloaded, so that compile command lines
    (and therefore remote cache keys) do not depend on the output base location.
    abs_path is the real location on disk, which is what clang reports for its
    builtin headers.
    """
    llvm_path = repository_ctx.os.environ.get("LLVM_PATH") or ""
    if llvm_path != "":
        return True, llvm_path, str(repository_ctx.path(llvm_path).realpath), ""

    success, error = _download_llvm(repository_ctx)
    if not success:
        return False, "", "", error

    return True, _LLVM_DIR, str(repository_ctx.path(_LLVM_DIR).realpath), ""

def _get_llvm_clang_include_dirs(repository_ctx, llvm_paths):
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

    for llvm_path in llvm_paths:
        for include_dir in ["include", "lib"]:
            include_dirs.append(llvm_path + "/" + include_dir)

    ret_include_dirs = []
    for path in include_dirs:
        ret_include_dirs.append(("            \"%s\"," % path))

    return ret_include_dirs

def _configure_oss_clang_toolchain(repository_ctx):
    build_file = "BUILD.bazel"

    success, llvm_path, llvm_abs_path, error = _get_llvm_info(repository_ctx)
    if not success:
        return False, error

    lld_path = repository_ctx.os.environ.get("LLD_PATH") or llvm_path

    # clang resolves its own real path to locate builtin headers, so they are
    # reported under the absolute path. Includes found relative to the execroot
    # show up under the repository-relative path.
    include_paths = [llvm_abs_path]
    if not llvm_path.startswith("/"):
        include_paths.append("external/{}/{}".format(repository_ctx.name, llvm_path))
    elif llvm_path != llvm_abs_path:
        include_paths.append(llvm_path)
    include_dirs = _get_llvm_clang_include_dirs(repository_ctx, include_paths)

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
        success, error_msg = _configure_oss_clang_toolchain(repository_ctx)
        if not success:
            fail(error_msg)
    else:
        repository_ctx.file("BUILD", "# Apple OSS LLVM Clang autoconfiguration was disabled because you're not on macOS")

mongo_apple_llvm_toolchain_config = repository_rule(
    environ = [
        "LLVM_PATH",  # Force re-compute if the user changed the location of the LLVM toolchain
        "LLD_PATH",  # Force re-compute if the user changed the location of the lld toolchain
    ],
    implementation = _apple_llvm_clang_cc_autoconf_impl,
    configure = True,
)

_ARCH_MAP = {
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
    "ppc64le": "@platforms//cpu:ppc64le",
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
    mongo_apple_llvm_toolchain_config(
        name = "mongo_apple_toolchain",
    )

setup_mongo_apple_toolchain_extension = module_extension(
    implementation = lambda ctx: setup_mongo_apple_toolchain(),
)
