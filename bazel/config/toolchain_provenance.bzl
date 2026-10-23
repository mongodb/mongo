"""Capture the configured tools without modifying server binaries."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain")
load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//bazel/config:configs.bzl", "compiler_type_provider", "remote_link_provider", "sdkroot_provider")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _target_platform_arch(ctx):
    is_macos = ctx.target_platform_has_constraint(ctx.attr._macos_constraint[platform_common.ConstraintValueInfo])
    arch_constraints = [
        (ctx.attr._x86_64_constraint, "x86_64"),
        (ctx.attr._aarch64_constraint, "arm64" if is_macos else "aarch64"),
        # platform.machine() reports arm64 on macOS and aarch64 on Linux.
        # Preserve that platform-specific spelling in build metadata.
        (ctx.attr._arm64_constraint, "arm64" if is_macos else "aarch64"),
        (ctx.attr._ppc64le_constraint, "ppc64le"),
        (ctx.attr._s390x_constraint, "s390x"),
        (ctx.attr._wasm32_constraint, "wasm32"),
    ]
    for constraint, arch in arch_constraints:
        if ctx.target_platform_has_constraint(constraint[platform_common.ConstraintValueInfo]):
            return arch
    fail("Unable to determine target architecture from the target platform")

def _target_platform_os(ctx):
    os_constraints = [
        (ctx.attr._linux_constraint, "linux"),
        (ctx.attr._macos_constraint, "macOS"),
        (ctx.attr._windows_constraint, "windows"),
        (ctx.attr._wasi_constraint, "wasi"),
    ]
    for constraint, os_name in os_constraints:
        if ctx.target_platform_has_constraint(constraint[platform_common.ConstraintValueInfo]):
            return os_name
    fail("Unable to determine target operating system from the target platform")

def _toolchain_provenance_impl(ctx):
    if _target_platform_os(ctx) not in ["linux", "macOS"]:
        output = ctx.actions.declare_file(ctx.label.name + ".json")
        ctx.actions.write(output, json.encode({"target_os": _target_platform_os(ctx), "captured": False}))
        return [DefaultInfo(files = depset([output]))]
    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    compiler_bin = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
    )
    compile_variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.cxxopts + ctx.fragments.cpp.copts,
    )
    link_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
        variables = compile_variables,
    )
    probe_link_flags = link_flags + ctx.attr.cpp_linkflags + ctx.fragments.cpp.linkopts
    compiler = ctx.attr._compiler_type[compiler_type_provider].compiler_type
    remote_macos_cross_link = (
        ctx.target_platform_has_constraint(ctx.attr._apple_cross[platform_common.ConstraintValueInfo]) and
        ctx.attr._macos_cross_local_container_actions[BuildSettingInfo].value and
        ctx.attr._remote_link[remote_link_provider].enabled
    )
    version_inputs = []
    for kind, action, driver, flags, mnemonic in [
        ("compiler", ACTION_NAMES.cpp_compile, compiler_bin, [], "ConfigHeaderGen"),
        ("linker", ACTION_NAMES.cpp_link_executable, cc_common.get_tool_for_action(
            feature_configuration = feature_configuration,
            action_name = ACTION_NAMES.cpp_link_executable,
        ), probe_link_flags, "CppLink"),
    ]:
        output = ctx.actions.declare_file(ctx.label.name + "_" + kind + "_version.json")
        probe_env = cc_common.get_environment_variables(
            feature_configuration = feature_configuration,
            action_name = action,
            variables = compile_variables,
        ) | {"SDKROOT": ctx.attr._sdkroot[sdkroot_provider].path}
        arguments = [
            "--kind",
            kind,
            "--driver",
            driver,
            "--compiler",
            compiler,
            "--flags",
            json.encode(flags),
            "--environment",
            json.encode(probe_env),
            "--output",
            output.path,
        ]

        executable = ctx.executable._version_probe
        inputs = [cc_toolchain.all_files]
        if kind == "linker" and ctx.attr.local_cross_link:
            # IBM cross builds compile remotely but link on the fetching host.
            # The composed toolchain keeps native link tools in a separate archive.
            executable = ctx.file._host_python
            arguments = [ctx.file._version_probe_script.path] + arguments
            inputs += [
                depset([ctx.file._version_probe_script]),
                ctx.attr._host_linker_files.files,
                ctx.attr._host_python_files.files,
            ]
        elif kind == "linker" and remote_macos_cross_link:
            # macOS host-wrapper toolchains resolve macOS Python, but CppLink
            # actions run on Linux when remote linking is enabled. Use the
            # Linux worker runtime already configured for remote IDL actions.
            python_interpreter = ctx.attr._linux_aarch64_python_interpreter
            python_files = ctx.attr._linux_aarch64_python_files
            if ctx.attr._macos_cross_linux_python_arch[BuildSettingInfo].value == "x86_64":
                python_interpreter = ctx.attr._linux_x86_64_python_interpreter
                python_files = ctx.attr._linux_x86_64_python_files
            interpreters = python_interpreter.files.to_list()
            if len(interpreters) != 1:
                fail("Linux Python runtime was requested for the remote linker version probe but is not available")
            executable = interpreters[0]
            arguments = [ctx.file._version_probe_script.path] + arguments
            inputs += [depset([ctx.file._version_probe_script]), python_files.files]
        ctx.actions.run(
            executable = executable,
            arguments = arguments,
            inputs = depset(transitive = inputs),
            outputs = [output],
            mnemonic = mnemonic,
            use_default_shell_env = True,
        )
        version_inputs.append(output)
    output = ctx.actions.declare_file(ctx.label.name + ".json")
    arguments = ctx.actions.args()
    arguments.add("--compiler-version-file", version_inputs[0])
    arguments.add("--linker-version-file", version_inputs[1])
    arguments.add("--target-os", _target_platform_os(ctx))
    arguments.add("--target-arch", _target_platform_arch(ctx))
    arguments.add("--toolchain-mode", "cross" if any([
        ctx.target_platform_has_constraint(target[platform_common.ConstraintValueInfo])
        for target in [ctx.attr._linux_cross, ctx.attr._apple_cross]
    ]) else "native")
    if ctx.attr.production_release:
        arguments.add("--production-release")
    arguments.add("--output", output)
    ctx.actions.run(
        executable = ctx.executable._version_manifest,
        arguments = [arguments],
        inputs = version_inputs,
        outputs = [output],
        # Match the execution-platform metadata actions used by cross builds.
        mnemonic = "ConfigHeaderGen",
    )
    return [DefaultInfo(files = depset([output]))]

toolchain_provenance = rule(
    _toolchain_provenance_impl,
    attrs = {
        "_compiler_type": attr.label(default = "//bazel/config:compiler_type"),
        "_linux_cross": attr.label(default = "//bazel/platforms:use_mongo_linux_cross_toolchain"),
        "_apple_cross": attr.label(default = "//bazel/platforms:use_mongo_apple_cross_toolchain"),
        "local_cross_link": attr.bool(default = False),
        "production_release": attr.bool(default = False),
        "_version_probe": attr.label(default = "//bazel/toolchains/cc:toolchain_version_probe", executable = True, cfg = "exec"),
        "_version_probe_script": attr.label(default = "//bazel/toolchains/cc:toolchain_version_probe.py", allow_single_file = True),
        "_host_python": attr.label(default = "@py_host//:interpreter", allow_single_file = True),
        "_host_python_files": attr.label(default = "@py_host//:files"),
        "_host_linker_files": attr.label(default = "@mongo_toolchain_v5//:all_files"),
        "_macos_cross_local_container_actions": attr.label(default = "//bazel/config:macos_cross_local_container_actions"),
        "_remote_link": attr.label(default = "//bazel/config:remote_link"),
        "_macos_cross_linux_python_arch": attr.label(default = "//bazel/config:macos_cross_linux_python_arch"),
        "_linux_aarch64_python_interpreter": attr.label(default = "@py_linux_arm64//:interpreter"),
        "_linux_aarch64_python_files": attr.label(default = "@py_linux_arm64//:files"),
        "_linux_x86_64_python_interpreter": attr.label(default = "@py_linux_x86_64//:interpreter"),
        "_linux_x86_64_python_files": attr.label(default = "@py_linux_x86_64//:files"),
        "cpp_linkflags": attr.string_list(),
        "_version_manifest": attr.label(default = "//bazel/toolchains/cc:toolchain_version_manifest", executable = True, cfg = "exec"),
        "_cc_toolchain": attr.label(default = "@rules_cc//cc:current_cc_toolchain"),
        "_sdkroot": attr.label(default = "//bazel/config:sdkroot"),
        "_linux_constraint": attr.label(default = "@platforms//os:linux"),
        "_macos_constraint": attr.label(default = "@platforms//os:macos"),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
        "_wasi_constraint": attr.label(default = "@platforms//os:wasi"),
        "_x86_64_constraint": attr.label(default = "@platforms//cpu:x86_64"),
        "_aarch64_constraint": attr.label(default = "@platforms//cpu:aarch64"),
        "_arm64_constraint": attr.label(default = "@platforms//cpu:arm64"),
        "_ppc64le_constraint": attr.label(default = "@platforms//cpu:ppc64le"),
        "_s390x_constraint": attr.label(default = "@platforms//cpu:s390x"),
        "_wasm32_constraint": attr.label(default = "@platforms//cpu:wasm32"),
    },
    fragments = ["cpp"],
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type", "@rules_python//python:toolchain_type"],
)
