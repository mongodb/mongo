# General starlark utility functions
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")

def write_target_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".gen_source_list")
    ctx.actions.write(
        out,
        "//" + ctx.label.package + ":" + ctx.attr.target_name,
    )
    return [
        DefaultInfo(
            files = depset([out]),
        ),
    ]

write_target = rule(
    write_target_impl,
    attrs = {
        "target_name": attr.string(
            doc = "the name of the target to record",
        ),
    },
)

def retry_download_and_extract(ctx, tries, **kwargs):
    sleep_time = 1
    for attempt in range(tries):
        is_retriable = attempt + 1 < tries
        result = ctx.download_and_extract(allow_fail = is_retriable, **kwargs)
        if result.success:
            return result
        else:
            print("Download failed (Attempt #%s), sleeping for %s seconds then retrying..." % (attempt + 1, sleep_time))
            ctx.execute(["sleep", str(sleep_time)])
            sleep_time *= 2

def retry_download(ctx, tries, **kwargs):
    sleep_time = 1
    for attempt in range(tries):
        is_retriable = attempt + 1 < tries
        result = ctx.download(allow_fail = is_retriable, **kwargs)
        if result.success:
            return result
        else:
            print("Download failed (Attempt #%s), sleeping for %s seconds then retrying..." % (attempt + 1, sleep_time))
            ctx.execute(["sleep", str(sleep_time)])
            sleep_time *= 2

def retry_execute(ctx, tries, arguments, **kwargs):
    """Runs a command with ctx.execute, retrying with exponential backoff.

    Returns the exec_result of the last attempt; the caller is responsible
    for checking return_code and failing with a useful message.
    """
    sleep_time = 1
    result = None
    for attempt in range(tries):
        result = ctx.execute(arguments, **kwargs)
        if result.return_code == 0:
            return result
        if attempt + 1 < tries:
            print("Command %s failed (Attempt #%s), sleeping for %s seconds then retrying..." % (arguments[0], attempt + 1, sleep_time))
            ctx.execute(["sleep", str(sleep_time)])
            sleep_time *= 2
    return result

def write_python_pyc_cache_prefix_customization(ctx, customization_file, pycache_dirname = "bazel_pycache"):
    """Write a site/usercustomize module to redirect .pyc writes to /tmp.

    Toolchain and runtime Python distributions often live under Bazel-managed paths
    (external repositories, runfiles, etc.). If Python writes `__pycache__/*.pyc`
    there, it can cause non-hermetic filesystem changes and Bazel cache churn.

    This helper writes either `sitecustomize.py` or `usercustomize.py` (callers
    choose the filename) to set `sys.pycache_prefix` to a temp directory.
    """
    ctx.file(
        customization_file,
        """
import os
import sys
import tempfile

# Prevent bytecode cache writes under Bazel-managed paths (external/, runfiles/).
sys.pycache_prefix = os.path.join(tempfile.gettempdir(), "{pycache_dirname}")
""".format(pycache_dirname = pycache_dirname),
    )

# A toolchain that deliberately matches nothing, for os/arch combinations where the
# mongo toolchain does not exist. It must not resolve, so that the platform's real
# toolchain wins instead.
_UNRESOLVABLE_TOOLCHAIN = """
toolchain(
    name = "mongo_toolchain",
    exec_compatible_with = ["@platforms//:incompatible"],
    target_compatible_with = ["@platforms//:incompatible"],
    toolchain = ":cc_mongo_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
"""

# A toolchain that always matches, backed by the same hollow cc_toolchain. Used only
# when the toolchain download is deliberately skipped (no_c++_toolchain=1) for
# analysis-only invocations such as the resmoke target cquery in
# generate_result_tasks.py.
_RESOLVABLE_TOOLCHAIN = """
toolchain(
    name = "mongo_toolchain",
    toolchain = ":cc_mongo_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
"""

# Minimal cc_toolchain_config_info. Declares no features and no real tools; it exists
# purely so toolchain resolution and cc_toolchain_alias analysis can succeed.
_NOOP_CC_TOOLCHAIN_CONFIG_BZL = """\
\"\"\"Hollow cc toolchain config for the no-op mongo toolchain.\"\"\"

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

def _impl(ctx):
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "mongo_noop_toolchain",
        host_system_name = "local",
        target_system_name = "local",
        target_cpu = "unknown",
        target_libc = "unknown",
        compiler = "unknown",
    )

noop_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
"""

def generate_noop_toolchain(ctx, substitutions, resolvable = False):
    """Generates a mongo toolchain repo with no downloaded payload.

    Args:
      ctx: the repository rule context.
      substitutions: unused; accepted so callers can pass the usual substitution dict.
      resolvable: when True, the stub toolchain carries no platform constraints so it
        always resolves. Pass True only when the download was skipped on purpose
        (no_c++_toolchain=1) for an analysis-only invocation. Leave False when the
        mongo toolchain genuinely does not exist for this os/arch, so that the real
        platform-specific toolchain (e.g. the apple one) is chosen instead of this stub.
    """

    # BUILD file is required for a no-op.
    # Keep a stub mongo_toolchain target so unconditional register_toolchains()
    # calls don't fail when the toolchain is intentionally skipped/unsupported.
    # Create a stub clang-format script so that targets referencing
    # //:clang_format (e.g. the format_multirun rule) can still build on
    # platforms where the mongo toolchain is unavailable (macOS, etc.).
    ctx.file("noop_cc_toolchain_config.bzl", _NOOP_CC_TOOLCHAIN_CONFIG_BZL)

    ctx.file(
        "clang_format_noop.sh",
        "#!/usr/bin/env bash\n# Stub: mongo toolchain clang-format is not available on this platform.\nexit 0\n",
        executable = True,
    )

    ctx.file(
        "BUILD.bazel",
        """
# {} not supported on this platform

load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("//:noop_cc_toolchain_config.bzl", "noop_cc_toolchain_config")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_files",
    srcs = [],
)

filegroup(
    name = "clang_tidy",
    srcs = [],
)

sh_binary(
    name = "clang_format",
    srcs = ["clang_format_noop.sh"],
)

filegroup(
    name = "llvm_symbolizer",
    srcs = [],
)

filegroup(
    name = "llvm_symbolizer_libs",
    srcs = [],
)

cc_toolchain(
    name = "cc_mongo_toolchain",
    all_files = ":all_files",
    ar_files = ":all_files",
    compiler_files = ":all_files",
    dwp_files = ":all_files",
    linker_files = ":all_files",
    objcopy_files = ":all_files",
    strip_files = ":all_files",
    toolchain_config = ":cc_mongo_toolchain_config",
)

noop_cc_toolchain_config(
    name = "cc_mongo_toolchain_config",
)
""".format(ctx.attr.version) + (_RESOLVABLE_TOOLCHAIN if resolvable else _UNRESOLVABLE_TOOLCHAIN),
    )

def get_toolchain_subs(ctx):
    if ctx.attr.os:
        os = ctx.attr.os
    else:
        os = ctx.os.name

    if ctx.attr.arch:
        arch = ctx.attr.arch
    else:
        arch = ctx.os.arch

    arch = ARCH_NORMALIZE_MAP[arch]

    version = ctx.attr.version

    distro = get_host_distro_major_version(ctx)

    def _toolchain_substitutions(platforms_arch, bazel_toolchain_cpu, arch, distro):
        return {
            "{platforms_arch}": platforms_arch,
            "{bazel_toolchain_cpu}": bazel_toolchain_cpu,
            "{exec_bazel_toolchain_cpu}": bazel_toolchain_cpu,
            "{target_bazel_toolchain_cpu}": bazel_toolchain_cpu,
            "{mongo_toolchain_constraint}": "@//bazel/platforms:use_mongo_toolchain",
            "{exec_distro_constraint}": "",
            "{target_distro_constraint}": "",
            "{toolchain_repo_name}": "mongo_toolchain_{version}".format(version = version),
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }

    if os != "linux":
        substitutions = _toolchain_substitutions("arm64", arch, arch, distro)
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)

    if arch == "aarch64":
        substitutions = _toolchain_substitutions("arm64", arch, arch, distro)
    elif arch == "x86_64":
        substitutions = _toolchain_substitutions("x86_64", "x86_64", arch, distro)
    elif arch == "ppc64le":
        substitutions = _toolchain_substitutions("ppc64le", "ppc64le", arch, distro)
    elif arch == "s390x":
        substitutions = _toolchain_substitutions("s390x", arch, arch, distro)
    else:
        substitutions = _toolchain_substitutions("none", arch, arch, distro)
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)

    substitutions["{toolchain_repo_dir}"] = "external/" + ctx.name

    return distro, arch, substitutions

def _get_amazon_linux_2023_minor_version(repository_ctx):
    """Returns the minor release number of Amazon Linux 2023, e.g. 3 for 2023.3, or None."""
    result = repository_ctx.execute([
        "sed",
        "-n",
        "s/Amazon Linux release 2023\\.\\([0-9]*\\)\\..*/\\1/p",
        "/etc/system-release",
    ])
    if result.return_code != 0 or not result.stdout.strip():
        return None
    return result.stdout.strip()

def get_host_distro_major_version(repository_ctx):
    _DISTRO_PATTERN_MAP = {
        "Ubuntu 18*": "ubuntu18",
        "Ubuntu 20*": "ubuntu20",
        "Ubuntu 22*": "ubuntu22",
        "Pop!_OS 22*": "ubuntu22",
        "Ubuntu 24*": "ubuntu24",
        "Amazon Linux 2": "amazon_linux_2",
        "Amazon Linux 2023": "amazon_linux_2023",
        "Debian GNU/Linux 10": "debian10",
        "Debian GNU/Linux 12": "debian12",
        "Debian GNU/Linux 13": "debian13",
        "Red Hat Enterprise Linux 8*": "rhel8",
        "Red Hat Enterprise Linux 9*": "rhel9",
        "Red Hat Enterprise Linux 10*": "rhel10",
        "Fedora*": "rhel10",
        "SLES 15*": "suse15",
        "SLES 16*": "suse16",
    }

    if repository_ctx.os.name != "linux":
        return None

    result = repository_ctx.execute([
        "sed",
        "-n",
        "/^\\(NAME\\|VERSION_ID\\)=/{s/[^=]*=//;s/\"//g;p}",
        "/etc/os-release",
    ])

    if result.return_code != 0:
        print("Failed to determine system distro, parsing os-release failed with the error: " + result.stderr)
        return None

    distro_seq = result.stdout.splitlines()
    if len(distro_seq) != 2:
        print("Failed to determine system distro, parsing os-release returned: " + result.stdout)
        return None

    distro_str = "{distro_name} {distro_version}".format(
        distro_name = distro_seq[0],
        distro_version = distro_seq[1],
    )

    for distro_pattern, simplified_name in _DISTRO_PATTERN_MAP.items():
        if "*" in distro_pattern:
            prefix_suffix = distro_pattern.split("*")
            if distro_str.startswith(prefix_suffix[0]) and distro_str.endswith(prefix_suffix[1]):
                return simplified_name
        elif distro_str == distro_pattern:
            if simplified_name == "amazon_linux_2023":
                minor = _get_amazon_linux_2023_minor_version(repository_ctx)
                if minor == "3":
                    return "amazon_linux_2023_3"
            return simplified_name
    return None
