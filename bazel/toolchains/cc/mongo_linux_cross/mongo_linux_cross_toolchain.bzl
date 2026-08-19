"""Repository rules for Linux s390x/ppc64le cross toolchains used with RBE."""

load("//bazel:utils.bzl", "generate_noop_toolchain", "retry_download_and_extract")

_TARGETS = [
    ("rhel8", "ppc64le"),
    ("rhel8", "s390x"),
    ("rhel9", "ppc64le"),
    ("rhel9", "s390x"),
]

_EXECS = [
    ("rhel9", "x86_64"),
    ("rhel9", "aarch64"),
]

def _repo_name(target_distro, target_arch, exec_distro, exec_arch):
    return "mongo_linux_cross_toolchain_v5_{}_{}_on_{}_{}".format(
        target_distro,
        target_arch,
        exec_distro,
        exec_arch,
    )

def _env_prefix(target_distro, target_arch, exec_distro = None, exec_arch = None):
    value = "MONGO_LINUX_CROSS_TOOLCHAIN_{}_{}".format(
        target_distro,
        target_arch,
    )
    if exec_distro and exec_arch:
        value += "_ON_{}_{}".format(exec_distro, exec_arch)
    return value.upper()

_ENV_SUFFIXES = ["URL", "SHA256", "STRIP_PREFIX"]

_ENV_VARS = [
    "{}_{}".format(_env_prefix(target_distro, target_arch), suffix)
    for target_distro, target_arch in _TARGETS
    for suffix in _ENV_SUFFIXES
] + [
    "{}_{}".format(_env_prefix(target_distro, target_arch, exec_distro, exec_arch), suffix)
    for target_distro, target_arch in _TARGETS
    for exec_distro, exec_arch in _EXECS
    for suffix in _ENV_SUFFIXES
]

def _platform_cpu(arch):
    if arch == "aarch64":
        return "arm64"
    return arch

def _env_candidates(ctx, suffix):
    full_prefix = _env_prefix(
        ctx.attr.target_distro,
        ctx.attr.target_arch,
        ctx.attr.exec_distro,
        ctx.attr.exec_arch,
    )
    target_prefix = _env_prefix(ctx.attr.target_distro, ctx.attr.target_arch)
    return [
        "{}_{}".format(full_prefix, suffix),
        "{}_{}".format(target_prefix, suffix),
    ]

def _first_env_value(ctx, suffix):
    for env_name in _env_candidates(ctx, suffix):
        value = ctx.os.environ.get(env_name, "")
        if value:
            return value
    return ""

def _substitutions(ctx):
    substitutions = {
        "{platforms_arch}": _platform_cpu(ctx.attr.target_arch),
        "{bazel_toolchain_cpu}": _platform_cpu(ctx.attr.target_arch),
        "{exec_bazel_toolchain_cpu}": _platform_cpu(ctx.attr.exec_arch),
        "{target_bazel_toolchain_cpu}": _platform_cpu(ctx.attr.target_arch),
        "{mongo_toolchain_constraint}": "@//bazel/platforms:use_mongo_linux_cross_toolchain",
        "{exec_distro_constraint}": "\"@//bazel/platforms:{}\",".format(ctx.attr.exec_distro),
        "{target_distro_constraint}": "\"@//bazel/platforms:{}\",".format(ctx.attr.target_distro),
        "{toolchain_repo_name}": ctx.name,
        "{toolchain_repo_dir}": "external/" + ctx.name,
        "{arch}": ctx.attr.target_arch,
        "{version}": ctx.attr.version,
        "{distro}": ctx.attr.target_distro,
    }

    # Cross-toolchain archives provide their own target sysroot. They do not
    # use the optional host RBE sysroot, but the shared Linux BUILD template
    # still requires these substitutions.
    substitutions.update({
        "{sysroot_load}": "",
        "{sysroot_defs}": """BUILTIN_SYSROOT = ""
EFFECTIVE_BUILTIN_INCLUDE_DIRS = COMMON_BUILTIN_INCLUDE_DIRECTORIES""",
        "{sysroot_all_files}": "",
    })
    return substitutions

def _linux_cross_toolchain_impl(ctx):
    substitutions = _substitutions(ctx)
    url = _first_env_value(ctx, "URL")
    sha256 = _first_env_value(ctx, "SHA256")
    strip_prefix = _first_env_value(ctx, "STRIP_PREFIX")

    if bool(url) != bool(sha256):
        fail(
            "{} requires both URL and SHA256. Set {} and {}.".format(
                ctx.name,
                " or ".join(_env_candidates(ctx, "URL")),
                " or ".join(_env_candidates(ctx, "SHA256")),
            ),
        )

    if not url:
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress(
            "{} is not configured. Set {} and {} to enable this Linux cross toolchain.".format(
                ctx.name,
                _env_candidates(ctx, "URL")[0],
                _env_candidates(ctx, "SHA256")[0],
            ),
        )
        return None

    if strip_prefix:
        retry_download_and_extract(
            ctx = ctx,
            tries = 5,
            url = url,
            sha256 = sha256,
            stripPrefix = strip_prefix,
        )
    else:
        retry_download_and_extract(
            ctx = ctx,
            tries = 5,
            url = url,
            sha256 = sha256,
        )

    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions = substitutions,
    )
    ctx.template(
        "mongo_toolchain_flags.bzl",
        ctx.attr.flags_tpl,
        substitutions = substitutions,
    )
    return None

linux_cross_toolchain_download = repository_rule(
    implementation = _linux_cross_toolchain_impl,
    configure = True,
    environ = _ENV_VARS,
    attrs = {
        "target_distro": attr.string(values = ["rhel8", "rhel9"], mandatory = True),
        "target_arch": attr.string(values = ["ppc64le", "s390x"], mandatory = True),
        "exec_distro": attr.string(values = ["rhel9"], mandatory = True),
        "exec_arch": attr.string(values = ["x86_64", "aarch64"], mandatory = True),
        "version": attr.string(values = ["v5"], mandatory = True),
        "flags_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_linux:mongo_toolchain_flags_v5.bzl",
            doc = "Label denoting the toolchain flags template.",
        ),
        "build_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_linux:mongo_toolchain.BUILD.tmpl",
            doc = "Label denoting the BUILD file template that gets installed in the repo.",
        ),
    },
)

def linux_cross_toolchain_repo_names():
    return [
        _repo_name(target_distro, target_arch, exec_distro, exec_arch)
        for target_distro, target_arch in _TARGETS
        for exec_distro, exec_arch in _EXECS
    ]

def setup_mongo_linux_cross_toolchains(register_toolchains = True):
    names = []
    for target_distro, target_arch in _TARGETS:
        for exec_distro, exec_arch in _EXECS:
            name = _repo_name(target_distro, target_arch, exec_distro, exec_arch)
            names.append(name)
            linux_cross_toolchain_download(
                name = name,
                target_distro = target_distro,
                target_arch = target_arch,
                exec_distro = exec_distro,
                exec_arch = exec_arch,
                version = "v5",
            )

    if register_toolchains:
        native.register_toolchains(*[
            "@{}//:mongo_toolchain".format(name)
            for name in names
        ])

def _setup_mongo_linux_cross_toolchains_extension(_ctx):
    setup_mongo_linux_cross_toolchains(register_toolchains = False)

setup_mongo_linux_cross_toolchains_extension = module_extension(
    implementation = _setup_mongo_linux_cross_toolchains_extension,
)
