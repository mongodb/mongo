"""GCC IBM target toolchains executed on matching RHEL ARM64 hosts."""

load("//bazel:utils.bzl", "retry_download_and_extract")
load("//bazel/toolchains/cc/mongo_linux:mongo_mold.bzl", "MOLD_MAP")
load(":mongo_toolchain_version_v5.bzl", "PPC64LE_GCC_TOOLCHAINS", "S390X_GCC_TOOLCHAINS")

_ENV = "MONGO_LINUX_CROSS_TOOLCHAIN"

def cross_selector(distro, arch = "s390x"):
    """Select only a same-release ARM64 execution toolchain."""
    return "{}_{}_on_{}_aarch64".format(distro, arch, distro)

def _cross_toolchain_impl(ctx):
    selector = ctx.os.environ.get(_ENV, "")
    if selector != cross_selector(ctx.attr.distro, ctx.attr.arch):
        ctx.file("BUILD.bazel", """toolchain(
    name = "mongo_toolchain",
    exec_compatible_with = ["@platforms//:incompatible"],
    target_compatible_with = ["@platforms//:incompatible"],
    toolchain = "@bazel_tools//tools/cpp:current_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""")
        return
    if ctx.os.name != "linux" or ctx.os.arch not in ["aarch64", "arm64"]:
        fail("{} GCC cross compilation requires a Linux ARM64 Bazel host".format(ctx.attr.arch))

    toolchains = PPC64LE_GCC_TOOLCHAINS if ctx.attr.arch == "ppc64le" else S390X_GCC_TOOLCHAINS
    info = toolchains[ctx.attr.distro]
    retry_download_and_extract(ctx, tries = 5, url = info["url"], sha256 = info["sha"])
    for path in [
        "stow/gcc-v5/bin/{}-mongodb-linux-gcc".format(ctx.attr.arch),
        "stow/gcc-v5/bin/{}-mongodb-linux-g++".format(ctx.attr.arch),
        "stow/gcc-v5/{}-mongodb-linux/bin/as".format(ctx.attr.arch),
    ]:
        if not ctx.path(path).exists:
            fail("GCC cross archive is missing {}".format(path))
    mold = MOLD_MAP["aarch64"]
    retry_download_and_extract(ctx, tries = 5, url = mold["url"], sha256 = mold["sha"])
    substitutions = {
        "{version}": "v5",
        "{arch}": ctx.attr.arch,
        "{bazel_toolchain_cpu}": "ppc" if ctx.attr.arch == "ppc64le" else ctx.attr.arch,
        "{sysroot_repo_name}": ctx.attr.arch + "_cross_sysroot",
        "{distro}": ctx.attr.distro,
        "{toolchain_repo_name}": ctx.name,
    }
    ctx.template("BUILD.bazel", ctx.attr.build_tpl, substitutions = substitutions)
    ctx.template("mongo_toolchain_flags.bzl", ctx.attr.flags_tpl, substitutions = substitutions)

cross_toolchain = repository_rule(
    implementation = _cross_toolchain_impl,
    environ = [_ENV],
    attrs = {
        "arch": attr.string(default = "s390x", values = ["s390x", "ppc64le"]),
        "distro": attr.string(values = ["rhel8", "rhel9", "rhel10"], mandatory = True),
        "build_tpl": attr.label(default = "//bazel/toolchains/cc/mongo_linux_cross:mongo_toolchain.BUILD.tmpl"),
        "flags_tpl": attr.label(default = "//bazel/toolchains/cc/mongo_linux_cross:mongo_toolchain_flags_v5.bzl"),
    },
)

def setup_s390x_cross_toolchains():
    """Register all candidates; download only the explicitly selected archive."""
    for distro in ["rhel8", "rhel9", "rhel10"]:
        name = "mongo_s390x_cross_toolchain_v5_" + distro
        cross_toolchain(name = name, distro = distro)
        native.register_toolchains("@" + name + "//:mongo_toolchain")

def setup_ppc64le_cross_toolchains():
    """Register PPC candidates without changing existing s390x archive pins."""
    for distro in ["rhel8", "rhel9", "rhel10"]:
        name = "mongo_ppc64le_cross_toolchain_v5_" + distro
        cross_toolchain(name = name, distro = distro, arch = "ppc64le")
        native.register_toolchains("@" + name + "//:mongo_toolchain")
