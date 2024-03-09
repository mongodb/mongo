load("//bazel/platforms:remote_execution_containers.bzl", "REMOTE_EXECUTION_CONTAINERS")
load("//bazel:utils.bzl", "get_host_distro_major_version")

_OS_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}

_ARCH_MAP = {
    "amd64": "@platforms//cpu:x86_64",
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
    "ppc64le": "@platforms//cpu:ppc64le",
    "s390x": "@platforms//cpu:s390x",
}

def _setup_local_config_platform(ctx):
    """
    Generates our own local_config_platform, overriding bazel's built in generation.

    This allows is to setup the exec_properties on this platform so a user can use remote execution
    without need to specify a specific platform.
    """

    if "win" in ctx.os.name:
        os = "windows"
    elif "mac" in ctx.os.name:
        os = "macos"
    else:
        os = "linux"

    arch = ctx.os.arch

    os_constraint = _OS_MAP[os]
    arch_constraint = _ARCH_MAP[arch]

    constraints = [os_constraint, arch_constraint]

    # So Starlark doesn't throw an indentation error when this gets injected.
    constraints_str = ",\n        ".join(['"%s"' % c for c in constraints])

    distro = get_host_distro_major_version(ctx)
    if arch == "x86_64":
        arch = "amd64"
    elif arch == "aarch64":
        arch = "arm64"

    if distro != None and distro + "_" + arch in REMOTE_EXECUTION_CONTAINERS:
        container_url = REMOTE_EXECUTION_CONTAINERS[distro + "_" + arch]["container-image"]
        print("Local host platform is configured to use this container if doing remote execution: {}".format(container_url))
        exec_props = """
    exec_properties = {
        "container-image": "%s",
        "dockerNetwork": "standard"
    },
""" % container_url
    else:
        exec_props = ""

    result = ctx.execute([
        "uname",
        "-r",
    ])
    version_numbers = result.stdout.split(".")
    if int(version_numbers[0]) > 4 or (int(version_numbers[0]) == 4 and int(version_numbers[1]) > 3):
        platform_constraints_str = constraints_str + ',\n        "@//bazel/platforms:kernel_version_4_4_or_greater"'
    else:
        platform_constraints_str = constraints_str + ',\n        "@//bazel/platforms:kernel_version_less_than_4_4"'

    substitutions = {
        "{constraints}": constraints_str,
        "{platform_constraints}": platform_constraints_str,
        "{exec_props}": exec_props,
    }

    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions = substitutions,
    )

    ctx.template(
        "constraints.bzl",
        ctx.attr.constraints_tpl,
        substitutions = substitutions,
    )

    return None

setup_local_config_platform = repository_rule(
    implementation = _setup_local_config_platform,
    attrs = {
        "build_tpl": attr.label(
            default = "//bazel/platforms:local_config_platform.BUILD",
            doc = "Template modeling the builtin local config platform build file.",
        ),
        "constraints_tpl": attr.label(
            default = "//bazel/platforms:local_config_platform_constraints.bzl",
            doc = "Template modeling the builtin local config platform constraints file.",
        ),
    },
)
