load("//bazel/platforms:remote_execution_containers.bzl", "REMOTE_EXECUTION_CONTAINERS")
load("//bazel/platforms:normalize.bzl", "ARCH_NORMALIZE_MAP", "ARCH_TO_PLATFORM_MAP", "OS_TO_PLATFORM_MAP")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain_version.bzl", "TOOLCHAIN_MAP")
load("//bazel:utils.bzl", "get_host_distro_major_version")

def _write_windows_cross_execution_platform(ctx):
    # Only the opted-in Windows wrapper supplies these values. Keep the normal
    # host platform unchanged, and override inherited remote properties on the
    # cross execution platform itself rather than remote-default properties.
    image = ctx.os.environ.get("MONGO_WINDOWS_CROSS_RBE_PLATFORM_IMAGE")
    if not image:
        return
    distro = ctx.os.environ.get("MONGO_WINDOWS_CROSS_RBE_PLATFORM_DISTRO")
    if distro not in REMOTE_EXECUTION_CONTAINERS:
        fail("Unsupported Windows cross execution distro: %s" % distro)
    pool = ctx.os.environ.get("MONGO_WINDOWS_CROSS_RBE_POOL") or "x86_64"
    if pool.lower() in ["arm64", "aarch64", "default"]:
        fail("Windows cross execution requires an x86_64 pool")
    ctx.file("windows_cross/BUILD.bazel", """
package(default_visibility = ["//visibility:public"])
platform(
    name = "exec",
    parents = [%s],
    exec_properties = %s,
)
""" % (
        json.encode("@//bazel/platforms:%s_amd64_windows_cross_exec" % distro),
        json.encode({"container-image": image, "Pool": pool, "dockerNetwork": "standard"}),
    ))

def _setup_local_config_platform(ctx):
    """
    Generates our own local_config_platform, overriding bazel's built in generation.

    This allows is to setup the exec_properties on this platform so a user can use remote execution
    without need to specify a specific platform.
    """

    _write_windows_cross_execution_platform(ctx)

    if "win" in ctx.os.name:
        os = "windows"
    elif "mac" in ctx.os.name:
        os = "macos"
    else:
        os = "linux"

    raw_arch = ctx.os.arch
    toolchain_arch = ARCH_NORMALIZE_MAP[raw_arch]

    os_constraint = OS_TO_PLATFORM_MAP[os]
    arch_constraint = ARCH_TO_PLATFORM_MAP[toolchain_arch]

    constraints = [os_constraint, arch_constraint]
    if os == "macos":
        constraints.append("@//bazel/platforms:use_mongo_native_apple_toolchain")

    # So Starlark doesn't throw an indentation error when this gets injected.
    constraints_str = ",\n        ".join(['"%s"' % c for c in constraints])

    # Keep the execution platform in sync with the image used by the wrapper. These
    # values are declared in the repository rule's environ list below so changing an
    # override also regenerates the platform and its action identity.
    distro = (
        ctx.os.environ.get("MONGO_HERMETIC_CONTAINER_DISTRO") or
        get_host_distro_major_version(ctx)
    )
    arch = toolchain_arch
    if arch == "x86_64":
        arch = "amd64"
    elif arch == "aarch64":
        arch = "arm64"

    # EngFlow's "default" pool is ARM64
    remote_execution_pool = "x86_64" if arch == "amd64" else "default"
    result = None
    platform_key = "{distro}_{arch}".format(distro = distro, arch = arch)
    toolchain_key = "{distro}_{arch}".format(distro = distro, arch = toolchain_arch)
    print("Trying to find toolchain for {}".format(toolchain_key))
    toolchain_exists = False
    for version in TOOLCHAIN_MAP:
        if toolchain_key in TOOLCHAIN_MAP[version]:
            toolchain_exists = True
            break

    cache_silo = '"cache-silo-key": "' + platform_key + '",' if ctx.os.environ.get("evergreen_remote_exec") == "off" else ""
    if ctx.os.environ.get("USE_NATIVE_TOOLCHAIN") == "1":
        constraints_str += ',\n        "@//bazel/platforms:use_native_toolchain"'
        exec_props = ""
        result = {"USE_NATIVE_TOOLCHAIN": "1"}
    elif distro != None and distro in REMOTE_EXECUTION_CONTAINERS:
        constraints_str += ',\n        "@//bazel/platforms:use_mongo_toolchain"'
        constraints_str += ',\n        "@//bazel/platforms:%s"' % (distro)
        container_url = (
            ctx.os.environ.get("MONGO_HERMETIC_CONTAINER_IMAGE") or
            REMOTE_EXECUTION_CONTAINERS[distro]["container-url"]
        )
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
    # Track these inputs so Bazel regenerates the platforms when their values change.
    environ = [
        # User override for the detected distro used to configure the normal host platform.
        # The Windows-cross wrapper also uses it when selecting its Linux execution distro.
        "MONGO_HERMETIC_CONTAINER_DISTRO",
        # User override for the RBE image on the normal host platform. Also serves as the
        # Windows-cross image fallback when MONGO_WINDOWS_CROSS_RBE_CONTAINER_IMAGE is unset.
        "MONGO_HERMETIC_CONTAINER_IMAGE",
        # Resolved docker:// image supplied by the Windows-cross wrapper. Its presence
        # enables the separate Linux execution platform used for Windows-cross actions.
        "MONGO_WINDOWS_CROSS_RBE_PLATFORM_IMAGE",
        # Resolved Linux distro supplied by that wrapper; selects the execution platform's
        # parent constraints and compiler flags while preserving the Windows host platform.
        "MONGO_WINDOWS_CROSS_RBE_PLATFORM_DISTRO",
        # EngFlow worker pool for Windows-cross actions. Defaults to x86_64; custom pools
        # must provide Linux x86_64 workers for the cross toolchain.
        "MONGO_WINDOWS_CROSS_RBE_POOL",
        # Set to 1 to select the native toolchain for the normal host platform instead of
        # configuring the managed MongoDB toolchain and its remote container image.
        "USE_NATIVE_TOOLCHAIN",
    ],
)
