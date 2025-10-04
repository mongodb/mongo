load("//bazel/platforms:remote_execution_containers.bzl", "REMOTE_EXECUTION_CONTAINERS")
load("//bazel/platforms:normalize.bzl", "ARCH_TO_PLATFORM_MAP", "OS_TO_PLATFORM_MAP")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain_version.bzl", "TOOLCHAIN_MAP")
load("//bazel:utils.bzl", "get_host_distro_major_version")

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

    os_constraint = OS_TO_PLATFORM_MAP[os]
    arch_constraint = ARCH_TO_PLATFORM_MAP[arch]

    constraints = [os_constraint, arch_constraint]

    # So Starlark doesn't throw an indentation error when this gets injected.
    constraints_str = ",\n        ".join(['"%s"' % c for c in constraints])

    distro = get_host_distro_major_version(ctx)
    if arch == "x86_64":
        arch = "amd64"
    elif arch == "aarch64":
        arch = "arm64"

    # EngFlow's "default" pool is ARM64
    remote_execution_pool = "x86_64" if arch == "amd64" else "default"
    result = None
    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = arch)
    print("Trying to find toolchain for {}".format(toolchain_key))
    toolchain_exists = False
    for version in TOOLCHAIN_MAP:
        if toolchain_key in TOOLCHAIN_MAP[version]:
            toolchain_exists = True
            break

    cache_silo = '"cache-silo-key": "' + toolchain_key + '",' if ctx.os.environ.get("evergreen_remote_exec") == "off" else ""
    if ctx.os.environ.get("USE_NATIVE_TOOLCHAIN"):
        exec_props = ""
        result = {"USE_NATIVE_TOOLCHAIN": "1"}
    elif distro != None and distro in REMOTE_EXECUTION_CONTAINERS:
        constraints_str += ',\n        "@//bazel/platforms:use_mongo_toolchain"'
        constraints_str += ',\n        "@//bazel/platforms:%s"' % (distro)
        container_url = REMOTE_EXECUTION_CONTAINERS[distro]["container-url"]
        web_url = REMOTE_EXECUTION_CONTAINERS[distro]["web-url"]
        dockerfile = REMOTE_EXECUTION_CONTAINERS[distro]["dockerfile"]
        print("Local host platform is configured to use this container if doing remote execution: {} built from {}".format(web_url, dockerfile))
        exec_props = """
    exec_properties = {
        "container-image": "%s",
        "dockerNetwork": "standard",
        "Pool": "%s",
        %s
    },
""" % (container_url, remote_execution_pool, cache_silo)
        result = {"DISTRO": distro}
    elif distro != None and toolchain_exists:
        constraints_str += ',\n        "@//bazel/platforms:use_mongo_toolchain"'
        constraints_str += ',\n        "@//bazel/platforms:%s"' % (distro)
        result = {"DISTRO": distro}
        exec_props = ""
    elif os == "windows":
        constraints_str += ',\n        "@//bazel/platforms:use_mongo_windows_toolchain_config"'
        result = {"USE_NATIVE_TOOLCHAIN": "1"}
        exec_props = ""
    else:
        result = {"USE_NATIVE_TOOLCHAIN": "1"}
        exec_props = ""

    platform_constraints_str = constraints_str

    substitutions = {
        "{constraints}": constraints_str,
        "{platform_constraints}": platform_constraints_str,
        "{exec_props}": exec_props,
    }

    ctx.template(
        "host/BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions = substitutions,
    )

    ctx.template(
        "host/constraints.bzl",
        ctx.attr.constraints_tpl,
        substitutions = substitutions,
    )

    ctx.template(
        "host/extension.bzl",
        ctx.attr.extension_tpl,
        substitutions = substitutions,
    )

    return result

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
        "extension_tpl": attr.label(
            default = "//bazel/platforms:local_config_platform_extension.bzl",
            doc = "Template modeling the builtin local config platform constraints file.",
        ),
    },
    environ = ["USE_NATIVE_TOOLCHAIN"],
)
