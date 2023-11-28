
_OS_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}

_ARCH_MAP = {
    "amd64": "@platforms//cpu:x86_64",
    "aarch64": "@platforms//cpu:arm64",
    "x86_64": "@platforms//cpu:x86_64",
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

    if os == "linux" and arch in ['amd64', 'aarch64', 'x86_64']:
        exec_props = """
    exec_properties = {
        # debian gcc based image contains the base our toolchain needs (glibc version and build-essentials)
        # https://hub.docker.com/layers/library/gcc/12.3-bookworm/images/sha256-6a3a5694d10299dbfb8747b98621abf4593bb54a5396999caa013cba0e17dd4f?context=explore
        "container-image": "docker://docker.io/library/gcc@sha256:6a3a5694d10299dbfb8747b98621abf4593bb54a5396999caa013cba0e17dd4f",
        "dockerNetwork": "standard"
    },
"""
    else:
        exec_props = ""

    substitutions = {
        "{constraints}": constraints_str,
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
