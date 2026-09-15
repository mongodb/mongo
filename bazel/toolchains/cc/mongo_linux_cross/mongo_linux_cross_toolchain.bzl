"""Repository rules for Linux s390x/ppc64le cross toolchains used with RBE."""

load("//bazel:utils.bzl", "generate_noop_toolchain", "retry_download_and_extract")
load(
    "//bazel/toolchains/cc/mongo_linux:sysroot_dump.bzl",
    "LINUX_CROSS_TOOLCHAIN_ENV_VAR",
)
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain_version_v5.bzl", "TOOLCHAIN_MAP_V5")

_TARGETS = [
    ("rhel8", "ppc64le"),
    ("rhel8", "s390x"),
    ("rhel9", "ppc64le"),
    ("rhel9", "s390x"),
    ("rhel10", "ppc64le"),
    ("rhel10", "s390x"),
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
    LINUX_CROSS_TOOLCHAIN_ENV_VAR,
] + [
    "{}_{}".format(_env_prefix(target_distro, target_arch), suffix)
    for target_distro, target_arch in _TARGETS
    for suffix in _ENV_SUFFIXES
] + [
    "{}_{}".format(_env_prefix(target_distro, target_arch, exec_distro, exec_arch), suffix)
    for target_distro, target_arch in _TARGETS
    for exec_distro, exec_arch in _EXECS
    for suffix in _ENV_SUFFIXES
]

_TARGET_TRIPLES = {
    "ppc64le": "ppc64le-mongodb-linux",
    "s390x": "s390x-mongodb-linux",
}

def _platform_cpu(arch):
    if arch == "aarch64":
        return "arm64"
    return arch

def _host_toolchain_paths(ctx):
    """Returns execution-root and toolchain-relative paths for the native archive."""

    # Tool paths in a cc_toolchain_config are resolved relative to the generated
    # cross repository. Command-line search paths, however, are resolved from the
    # execution root. Resolve the host repository's canonical name once so both
    # forms point at the same declared files when the link runs remotely or in the
    # native persistent container.
    #
    # The ``../`` form is only consumed by the linker driver's action-config tool
    # path, which is safe because link actions always run locally in cross mode;
    # it can never resolve on RBE workers or in sandboxes, which only receive
    # declared inputs. Tools that may run there must use the in-repository
    # ``host_tools/`` symlinks instead.
    host_tool = str(ctx.path(Label("@mongo_toolchain_v5//:v5/bin/clang")))
    host_repo_name = host_tool.rsplit("/external/", 1)[-1].split("/", 1)[0]
    return "external/" + host_repo_name, "../" + host_repo_name

def _host_linker_substitutions(ctx):
    """Returns host-native paths for local link and output actions.

    The composed cross repository contains execution-architecture tools at its
    root and target-architecture runtime files below ``target/``.  Local link,
    archive, and debug actions instead run in the native IBM container, so they
    need the corresponding tools from the host's regular mongo toolchain.  The
    ``remote_link_enabled`` setting selects the execution archive for the
    explicit remote-link mode and the host archive for the default local mode.
    """
    host_repo, host_tool_repo = _host_toolchain_paths(ctx)
    target_repo = "external/" + ctx.name
    host_linker_select = "@//bazel/config:remote_link_enabled"

    # The driver must be executable on the host, but its CRT objects and GCC
    # runtime are for the target architecture. Keep the host LLVM directory in
    # the -B list so the driver can find ld.lld, while pointing GCC's runtime
    # search at the target archive rather than the native host archive.
    target_linker_bin_dirs = [
        target_repo + "/target/stow/gcc-v5/lib/gcc/{}-mongodb-linux/14.2.0".format(ctx.attr.target_arch),
        host_repo + "/stow/llvm-v5/bin",
    ]

    def _host_tool_select(path):
        return (
            "select({\n" +
            "    \"{}\": \"\",\n".format(host_linker_select) +
            "    \"//conditions:default\": {},\n".format(repr(path)) +
            "})"
        )

    def _host_files_select():
        return (
            "select({\n" +
            "    \"{}\": \":all_files\",\n".format(host_linker_select) +
            "    \"//conditions:default\": \":host_output_tools\",\n" +
            "})"
        )

    return {
        # The remote_link build setting selects the execution-architecture
        # compiler for explicit remote links. The default branch is used by
        # local persistent-container links and points at the native host archive.
        "{host_linker_files}": "select({\n" +
                               "    \"{}\": [],\n".format(host_linker_select) +
                               "    \"//conditions:default\": [\"@mongo_toolchain_v5//:all_files\"],\n" +
                               "})",
        "{host_linker_bin_dirs}": "select({\n" +
                                  "    \"{}\": [],\n".format(host_linker_select) +
                                  "    \"//conditions:default\": {},\n".format(repr(target_linker_bin_dirs)) +
                                  "})",
        "{host_linker_resource_dir}": "select({\n" +
                                      "    \"{}\": \"\",\n".format(host_linker_select) +
                                      "    \"//conditions:default\": {},\n".format(repr(host_repo + "/stow/llvm-v5/lib/clang/19/")) +
                                      "})",
        "{host_linker_tool_path}": "select({\n" +
                                   "    \"{}\": \"\",\n".format(host_linker_select) +
                                   "    \"//conditions:default\": {},\n".format(repr(host_tool_repo + "/stow/llvm-v5/bin/clang++")) +
                                   "})",
        "{host_linker_toolchain_repo_dir}": "select({\n" +
                                            "    \"{}\": \"\",\n".format(host_linker_select) +
                                            "    \"//conditions:default\": {},\n".format(repr(host_repo)) +
                                            "})",
        # Output tools are selected independently from the compiler tool paths.
        # The empty remote branch preserves the execution archive's tools when
        # CppLink and its output actions are explicitly sent to RBE.
        # Tool paths in a cc_toolchain_config are relative to this generated
        # external repository. Keep the native tools inside the repository as
        # symlinks (created by _linux_cross_toolchain_impl) so Bazel accepts a
        # normalized path and includes the host files in local action sandboxes.
        "{host_ar_tool_path}": _host_tool_select("host_tools/v5/bin/ar"),
        "{host_dwp_tool_path}": _host_tool_select("host_tools/v5/bin/llvm-dwp"),
        "{host_objcopy_tool_path}": _host_tool_select("host_tools/v5/bin/llvm-objcopy"),
        "{host_strip_tool_path}": _host_tool_select("host_tools/v5/bin/strip"),
        "{host_ar_files}": _host_files_select(),
        "{host_dwp_files}": _host_files_select(),
        "{host_objcopy_files}": _host_files_select(),
        "{host_strip_files}": _host_files_select(),
    }

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

def _selector(ctx):
    return "{}_{}_on_{}_{}".format(
        ctx.attr.target_distro,
        ctx.attr.target_arch,
        ctx.attr.exec_distro,
        ctx.attr.exec_arch,
    )

def _exec_toolchain_loads():
    return """load(
    ":mongo_exec_toolchain_flags.bzl",
    EXEC_CLANG_INCLUDE_DIRS = "CLANG_INCLUDE_DIRS",
    EXEC_COMMON_BINDIRS = "COMMON_BINDIRS",
    EXEC_COMMON_BUILTIN_INCLUDE_DIRECTORIES = "COMMON_BUILTIN_INCLUDE_DIRECTORIES",
    EXEC_COMMON_INCLUDE_DIRECTORIES = "COMMON_INCLUDE_DIRECTORIES",
    EXEC_COMMON_LINK_FLAGS = "COMMON_LINK_FLAGS",
    EXEC_CLANG_RESOURCE_DIR = "clang_resource_dir",
)"""

def _exec_toolchain_fragment(ctx):
    exec_cpu = _platform_cpu(ctx.attr.exec_arch)
    return """EXEC_LINK_FLAGS = ["-L" + flag for flag in EXEC_COMMON_LINK_FLAGS]

mongo_linux_cc_toolchain_config(
    name = "cc_clang_exec_toolchain_config",
    bin_dirs = EXEC_COMMON_BINDIRS,
    compiler = "clang",
    dbg = IS_DEBUG_BUILD,
    linker = CHOSEN_LINKER,
    mold_bin_dir = "",
    toolchain_repo_dir = "{toolchain_repo_dir}",
    distro = "{exec_distro}",
    cpu = "{exec_cpu}",
    builtin_sysroot = "",
    clang_resource_dir = EXEC_CLANG_RESOURCE_DIR("{toolchain_repo_dir}"),
    cxx_builtin_include_directories = EXEC_COMMON_BUILTIN_INCLUDE_DIRECTORIES,
    extra_ldflags = EXEC_LINK_FLAGS,
    includes = EXEC_COMMON_INCLUDE_DIRECTORIES + EXEC_COMMON_BUILTIN_INCLUDE_DIRECTORIES + EXEC_CLANG_INCLUDE_DIRS,
    tool_paths = get_mongo_toolchain_tool_paths("{version}", "clang"),
    toolchain_repo_name = "{toolchain_repo_name}",
    toolchain_identifier = "clang_exec_toolchain",
    verbose = True,
    libvoidstar = LIBVOIDSTAR,
    linkstatic = LINKSTATIC_ENABLED,
    shared_archive = SHARED_ARCHIVE_ENABLED,
    dwarf_version = DWARF_VERSION,
    fission = FISSION_ENABLED,
    debug_level = DEBUG_LEVEL,
    disable_debug_symbols = DISABLE_DEBUG_SYMBOLS,
    optimization_level = feature_attrs[FEATURES_ATTR_NAMES.OPT_LEVEL],
    pgo_profile_generate = PGO_PROFILE_GENERATE_ENABLED,
    pgo_profile_use = PGO_PROFILE_USE_ENABLED,
    cspgo_profile_generate = CSPGO_PROFILE_GENERATE_ENABLED,
    cspgo_profile_use = CSPGO_PROFILE_USE_ENABLED,
    propeller_profile_generate = PROPELLER_PROFILE_GENERATE_ENABLED,
    distributed_thin_lto = DTLTO_ENABLED,
    any_sanitizer_enabled = ANY_SANITIZER_ENABLED,
    asan_enabled = ASAN_ENABLED,
    asan_denylist = ASAN_DENYLIST,
    fsan_enabled = FSAN_ENABLED,
    msan_enabled = MSAN_ENABLED,
    msan_denylist = MSAN_DENYLIST,
    tsan_enabled = TSAN_ENABLED,
    tsan_denylist = TSAN_DENYLIST,
    ubsan_enabled = UBSAN_ENABLED,
    ubsan_denylist = UBSAN_DENYLIST,
    is_aarch64 = {is_aarch64},
    is_ppc64le = False,
    is_s390x = False,
    is_x86_64 = {is_x86_64},
    internal_thin_lto_enabled = INTERNAL_THIN_LTO_ENABLED,
    coverage_enabled = COVERAGE_ENABLED,
    compress_debug_enabled = COMPRESS_DEBUG_ENABLED,
    warnings_as_errors_enabled = WARNINGS_AS_ERRORS_ENABLED,
    global_defines = MONGO_GLOBAL_DEFINES,
)

cc_toolchain(
    name = "cc_mongo_exec_toolchain",
    all_files = ":all_files",
    ar_files = ":all_files",
    compiler_files = ":all_files",
    dwp_files = ":all_files",
    linker_files = ":linker_files",
    objcopy_files = ":all_files",
    strip_files = ":all_files",
    toolchain_config = ":cc_clang_exec_toolchain_config",
)

toolchain(
    name = "mongo_exec_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{exec_cpu}",
        "@//bazel/platforms:use_mongo_toolchain",
        "@//bazel/platforms:{exec_distro}",
        "@//bazel/platforms:use_mongo_linux_cross_execution",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{exec_cpu}",
        "@//bazel/platforms:use_mongo_toolchain",
        "@//bazel/platforms:{exec_distro}",
    ],
    target_settings = ["@//bazel/config:mongo_toolchain_{version}"],
    toolchain = ":cc_mongo_exec_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)

# WheelBuild runs in an execution group whose target platform is the host
# platform. Select the composed target toolchain there explicitly: its compiler
# binaries are from the execution archive, while its sysroot and target flags
# describe the IBM target. The dedicated toolchain type keeps this candidate
# out of ordinary host-tool execution actions, while the marker keeps it out of
# native and local release toolchain resolution.
toolchain(
    name = "mongo_exec_toolchain_for_pycross",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:{exec_cpu}",
        "@//bazel/platforms:use_mongo_toolchain",
        "@//bazel/platforms:{exec_distro}",
        "@//bazel/platforms:use_mongo_linux_cross_execution",
    ],
    # WheelBuild's target platform is the host platform, not the IBM target
    # platform.  Keep this candidate target-compatible with that host platform;
    # the cross marker below and the explicit toolchain registration in the
    # cross configs keep it out of native/local-release resolution.
    target_compatible_with = [],
    target_settings = [
        "@//bazel/config:mongo_toolchain_{version}",
        "@//bazel/config:mongo_ibm_cross",
    ],
    toolchain = ":cc_mongo_toolchain",
    toolchain_type = "@//bazel/toolchains:mongo_pycross_cc_toolchain_type",
)""".format(
        exec_cpu = exec_cpu,
        exec_distro = ctx.attr.exec_distro,
        is_aarch64 = ctx.attr.exec_arch == "aarch64",
        is_x86_64 = ctx.attr.exec_arch == "x86_64",
        toolchain_repo_dir = "external/" + ctx.name,
        toolchain_repo_name = ctx.name,
        version = ctx.attr.version,
    )

def _generate_noop_cross_toolchain(ctx, substitutions):
    generate_noop_toolchain(ctx, substitutions)
    build_file = ctx.read(ctx.path("BUILD.bazel"))
    ctx.file(
        "BUILD.bazel",
        build_file + """

toolchain(
    name = "mongo_exec_toolchain",
    exec_compatible_with = ["@platforms//:incompatible"],
    target_compatible_with = ["@platforms//:incompatible"],
    toolchain = "@bazel_tools//tools/cpp:current_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)

toolchain(
    name = "mongo_exec_toolchain_for_pycross",
    exec_compatible_with = ["@platforms//:incompatible"],
    target_compatible_with = ["@platforms//:incompatible"],
    toolchain = "@bazel_tools//tools/cpp:current_cc_toolchain",
    toolchain_type = "@//bazel/toolchains:mongo_pycross_cc_toolchain_type",
)

""",
    )

def _substitutions(ctx, use_composed_toolchain):
    substitutions = {
        "{platforms_arch}": _platform_cpu(ctx.attr.target_arch),
        "{bazel_toolchain_cpu}": _platform_cpu(ctx.attr.target_arch),
        "{exec_bazel_toolchain_cpu}": _platform_cpu(ctx.attr.exec_arch),
        "{target_bazel_toolchain_cpu}": _platform_cpu(ctx.attr.target_arch),
        "{mongo_toolchain_constraint}": "@//bazel/platforms:use_mongo_linux_cross_toolchain",
        "{exec_distro_constraint}": "\"@//bazel/platforms:{}\",".format(ctx.attr.exec_distro),
        "{target_distro_constraint}": "\"@//bazel/platforms:{}\",".format(ctx.attr.target_distro),
        "{target_triple}": _TARGET_TRIPLES[ctx.attr.target_arch],
        "{extra_target_settings}": "\"@//bazel/config:compiler_type_clang\",",
        "{extra_loads}": _exec_toolchain_loads(),
        "{extra_toolchains}": _exec_toolchain_fragment(ctx),
        "{host_linker_files}": "[]",
        "{host_linker_bin_dirs}": "[]",
        "{host_linker_resource_dir}": "\"\"",
        "{host_linker_tool_path}": "\"\"",
        "{host_linker_toolchain_repo_dir}": "\"\"",
        "{host_ar_tool_path}": "\"\"",
        "{host_dwp_tool_path}": "\"\"",
        "{host_objcopy_tool_path}": "\"\"",
        "{host_strip_tool_path}": "\"\"",
        "{host_ar_files}": "\":all_files\"",
        "{host_dwp_files}": "\":all_files\"",
        "{host_objcopy_files}": "\":all_files\"",
        "{host_strip_files}": "\":all_files\"",
        "{toolchain_repo_name}": ctx.name,
        "{toolchain_repo_dir}": "external/" + ctx.name,
        "{arch}": ctx.attr.target_arch,
        "{version}": ctx.attr.version,
        "{distro}": ctx.attr.target_distro,
    }

    if use_composed_toolchain:
        substitutions.update(_host_linker_substitutions(ctx))
        substitutions.update({
            "{sysroot_load}": """load("@rbe_sysroot//:sysroot_info.bzl", "SYSROOT_PATH")""",
            "{sysroot_defs}": """SYSROOT_BUILTIN_INCLUDE_DIRECTORIES = (
    COMMON_BUILTIN_INCLUDE_DIRECTORIES +
    ["%sysroot%" + d for d in COMMON_BUILTIN_INCLUDE_DIRECTORIES if d.startswith("/")] +
    [SYSROOT_PATH + d for d in COMMON_BUILTIN_INCLUDE_DIRECTORIES if d.startswith("/")]
)

SYSROOT_SYSTEM_INCLUDE_DIRECTORIES = [
    SYSROOT_PATH + d
    for d in COMMON_BUILTIN_INCLUDE_DIRECTORIES
    if d.startswith("/")
]

BUILTIN_SYSROOT = SYSROOT_PATH
EFFECTIVE_BUILTIN_INCLUDE_DIRS = SYSROOT_BUILTIN_INCLUDE_DIRECTORIES
EFFECTIVE_SYSTEM_INCLUDE_DIRS = SYSROOT_SYSTEM_INCLUDE_DIRECTORIES""",
            "{sysroot_all_files}": """ + ["@rbe_sysroot//:sysroot_files"]""",
        })
    else:
        # A complete override archive may provide its own compiler wrappers and
        # sysroot, preserving the original milestone scaffold's escape hatch.
        substitutions.update({
            "{sysroot_load}": "",
            "{sysroot_defs}": """BUILTIN_SYSROOT = ""
EFFECTIVE_BUILTIN_INCLUDE_DIRS = COMMON_BUILTIN_INCLUDE_DIRECTORIES
EFFECTIVE_SYSTEM_INCLUDE_DIRS = COMMON_BUILTIN_INCLUDE_DIRECTORIES""",
            "{sysroot_all_files}": "",
        })
    return substitutions

def _linux_cross_toolchain_impl(ctx):
    selected = ctx.os.environ.get(LINUX_CROSS_TOOLCHAIN_ENV_VAR, "")
    if selected != _selector(ctx):
        _generate_noop_cross_toolchain(ctx, _substitutions(ctx, False))
        return None

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

    use_composed_toolchain = not url
    if url:
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
    else:
        exec_key = "{}_{}".format(ctx.attr.exec_distro, ctx.attr.exec_arch)
        target_key = "{}_{}".format(ctx.attr.target_distro, ctx.attr.target_arch)
        exec_toolchain = TOOLCHAIN_MAP_V5[exec_key]
        target_toolchain = TOOLCHAIN_MAP_V5[target_key]

        ctx.report_progress("Downloading {} execution toolchain".format(exec_key))
        retry_download_and_extract(
            ctx = ctx,
            tries = 5,
            url = exec_toolchain["url"],
            sha256 = exec_toolchain["sha"],
        )
        ctx.report_progress("Downloading {} target runtime".format(target_key))
        retry_download_and_extract(
            ctx = ctx,
            output = "target",
            tries = 5,
            url = target_toolchain["url"],
            sha256 = target_toolchain["sha"],
        )

    if use_composed_toolchain:
        # Keep host output tools under the generated cross repository as
        # declared inputs. A direct ``external/<host-repo>`` tool path is
        # interpreted below this external repository rather than at the exec
        # root, and an ``../`` escape path only resolves in the local
        # execution context; sandboxes and RBE workers only receive declared
        # inputs, so neither form can serve tools that may run there.
        # Repository symlinks preserve the host toolchain files as declared
        # inputs while giving the action a normalized path that resolves in
        # both local containers and RBE workers.
        # The execution archive is normally used by remote actions, but Bazel
        # can still select its toolchain-relative output-tool paths for local
        # ExtractDebugInfo/StripDebugInfo actions.  Those actions run in the
        # native IBM container, so an unwrapped x86/aarch64 ELF would fail with
        # Exec format error.  Cargo build scripts have the same issue for ar.
        # Dispatch every output tool by the process architecture: local IBM
        # actions use the native Mongo toolchain, while RBE workers use the
        # downloaded execution archive.
        for tool in [
            "ar",
            "llvm-cov",
            "llvm-dwp",
            "llvm-objcopy",
            "llvm-profdata",
            "strip",
        ]:
            tool_path = "v5/bin/{}".format(tool)
            exec_tool_path = tool_path + ".exec"
            move_result = ctx.execute([
                "mv",
                str(ctx.path(tool_path)),
                str(ctx.path(exec_tool_path)),
            ])
            if move_result.return_code != 0:
                fail("Unable to prepare the portable {} wrapper: {}".format(tool, move_result.stderr))
            ctx.symlink(
                Label("@mongo_toolchain_v5//:v5/bin/{}".format(
                    "llvm-ar" if tool == "ar" else tool,
                )),
                "v5/bin/{}.native".format(tool),
            )
            wrapper = """#!/bin/sh
set -eu
case "$(uname -m)" in
    ppc64le|s390x)
        exec "$(dirname "$0")/{tool}.native" "$@"
        ;;
    *)
        # GNU binutils in the execution archive is dynamically linked against
        # the bundled libbfd.  Bazel's action sandbox does not inherit the
        # repository's host environment, so make both archive layouts
        # discoverable before invoking the foreign binary.  The paths cover
        # both v5/bin and host_tools/v5/bin layouts, including binutils and
        # GCC runtime directories.
        tool_dir="$(dirname "$0")"
        for lib_dir in \
            "$tool_dir/../lib" \
            "$tool_dir/../lib64" \
            "$tool_dir/../../stow/binutils-v5/lib" \
            "$tool_dir/../../stow/binutils-v5/lib64" \
            "$tool_dir/../../stow/gcc-v5/lib" \
            "$tool_dir/../../stow/gcc-v5/lib64" \
            "$tool_dir/../../../stow/binutils-v5/lib" \
            "$tool_dir/../../../stow/binutils-v5/lib64" \
            "$tool_dir/../../../stow/gcc-v5/lib" \
            "$tool_dir/../../../stow/gcc-v5/lib64" \
            "$tool_dir/../../../v5/lib" \
            "$tool_dir/../../../v5/lib64"; do
            if [ -d "$lib_dir" ]; then
                LD_LIBRARY_PATH="$lib_dir${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
                export LD_LIBRARY_PATH
            fi
        done
        exec "$tool_dir/{tool}.exec" "$@"
        ;;
esac
""".format(tool = tool)
            ctx.file(tool_path, wrapper, executable = True)

            # Keep a normalized host-output path for the local-link branch.
            # It has its own dispatch siblings because invoking a script via a
            # symlink changes $0 and therefore its dirname.
            ctx.symlink(
                Label("@mongo_toolchain_v5//:v5/bin/{}".format(
                    "llvm-ar" if tool == "ar" else tool,
                )),
                "host_tools/v5/bin/{}.native".format(tool),
            )
            ctx.symlink(
                exec_tool_path,
                "host_tools/v5/bin/{}.exec".format(tool),
            )
            ctx.file(
                "host_tools/v5/bin/{}".format(tool),
                wrapper,
                executable = True,
            )

    ctx.file(
        "openssl_overrides.bzl",
        "OPENSSL_LINK_DIRS = []\nOPENSSL_INCLUDE_DIRS = []\n",
    )
    substitutions = _substitutions(ctx, use_composed_toolchain)

    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions = substitutions,
    )
    ctx.template(
        "mongo_toolchain_flags.bzl",
        ctx.attr.composed_flags_tpl if use_composed_toolchain else ctx.attr.flags_tpl,
        substitutions = substitutions,
    )
    exec_substitutions = dict(substitutions)
    exec_substitutions["{arch}"] = ctx.attr.exec_arch
    ctx.template(
        "mongo_exec_toolchain_flags.bzl",
        ctx.attr.flags_tpl,
        substitutions = exec_substitutions,
    )
    return None

linux_cross_toolchain_download = repository_rule(
    implementation = _linux_cross_toolchain_impl,
    configure = True,
    environ = _ENV_VARS,
    attrs = {
        "target_distro": attr.string(values = ["rhel8", "rhel9", "rhel10"], mandatory = True),
        "target_arch": attr.string(values = ["ppc64le", "s390x"], mandatory = True),
        "exec_distro": attr.string(values = ["rhel9"], mandatory = True),
        "exec_arch": attr.string(values = ["x86_64", "aarch64"], mandatory = True),
        "version": attr.string(values = ["v5"], mandatory = True),
        "flags_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_linux:mongo_toolchain_flags_v5.bzl",
            doc = "Flags template for an optional complete archive override.",
        ),
        "composed_flags_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_linux_cross:mongo_linux_cross_toolchain_flags_v5.bzl",
            doc = "Flags template for the composed execution/target toolchain.",
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
        native.register_toolchains(*([
            "@{}//:mongo_toolchain".format(name)
            for name in names
        ] + [
            "@{}//:mongo_exec_toolchain".format(name)
            for name in names
        ]))

def _setup_mongo_linux_cross_toolchains_extension(_ctx):
    setup_mongo_linux_cross_toolchains(register_toolchains = False)

setup_mongo_linux_cross_toolchains_extension = module_extension(
    implementation = _setup_mongo_linux_cross_toolchains_extension,
)
