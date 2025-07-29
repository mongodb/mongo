# General starlark utility functions
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP")

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

def generate_noop_toolchain(ctx, substitutions):
    # BUILD file is required for a no-op
    ctx.file(
        "BUILD.bazel",
        "# {} not supported on this platform".format(ctx.attr.version),
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

    if os != "linux":
        substitutions = {
            "{platforms_arch}": "arm64",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)

    if arch == "aarch64":
        substitutions = {
            "{platforms_arch}": "arm64",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }
    elif arch == "x86_64":
        substitutions = {
            "{platforms_arch}": "x86_64",
            "{bazel_toolchain_cpu}": "x86_64",
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }
    elif arch == "ppc64le":
        substitutions = {
            "{platforms_arch}": "ppc64le",
            "{bazel_toolchain_cpu}": "ppc",
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }
    elif arch == "s390x":
        substitutions = {
            "{platforms_arch}": "s390x",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }
    else:
        substitutions = {
            "{platforms_arch}": "none",
            "{bazel_toolchain_cpu}": arch,
            "{arch}": arch,
            "{version}": version,
            "{distro}": distro,
        }
        generate_noop_toolchain(ctx, substitutions)
        ctx.report_progress("mongo toolchain not supported on " + os + " and " + arch)

    return distro, arch, substitutions

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
        "Red Hat Enterprise Linux 8*": "rhel8",
        "Red Hat Enterprise Linux 9*": "rhel9",
        "SLES 15*": "suse15",
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
            return simplified_name
    return None
