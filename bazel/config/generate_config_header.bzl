load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain")
load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//bazel/config:configs.bzl", "sdkroot_provider")
load("//bazel/config:py_action_env.bzl", "py_action_env_windows_dll_path")
load("//bazel:utils.bzl", "write_target")

def _strip_sysroot_flags(flags):
    """Remove --sysroot and sysroot-debug-prefix-map flags.

    When building locally with an RBE sysroot, the toolchain adds --sysroot
    and -fdebug-prefix-map flags that are not present in RBE builds. Strip
    them so the embedded build-info strings are identical either way.
    """
    result = []
    skip_next = False
    for flag in flags:
        if skip_next:
            skip_next = False
            continue
        if flag == "--sysroot":
            # --sysroot <value> is two separate entries
            skip_next = True
            continue
        if flag.startswith("--sysroot=") or flag.startswith("--sysroot "):
            continue
        if flag.startswith("-fdebug-prefix-map=") and flag.endswith("=/"):
            continue
        result.append(flag)
    return result

def _target_platform_arch(ctx):
    is_macos = ctx.target_platform_has_constraint(ctx.attr._macos_constraint[platform_common.ConstraintValueInfo])
    arch_constraints = [
        (ctx.attr._x86_64_constraint, "x86_64"),
        (ctx.attr._aarch64_constraint, "aarch64"),
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

def generate_config_header_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    input = ctx.attr.template.files.to_list()[0].path
    checks = ctx.attr.checks.files.to_list()[0].path

    # Generate compiler flags we need to make clang/gcc/msvc actually compile.
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
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
    compiler_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = compile_variables,
    )

    # The config header generation should not fail due to compiler warnings,
    # so remove any flags that would treat warnings as errors.
    compiler_flags = [
        flag
        for flag in compiler_flags
        if not (
            flag == "-Werror" or
            flag.startswith("-Werror") or
            flag == "/WX"
        )
    ]
    link_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
        variables = compile_variables,
    )

    # Strip sysroot-related flags so that the embedded build info is
    # identical regardless of whether a local RBE sysroot is used.
    compiler_flags = _strip_sysroot_flags(compiler_flags)
    link_flags = _strip_sysroot_flags(link_flags)
    env_flags = cc_common.get_environment_variables(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = compile_variables,
    )

    # Native Windows py_binary launchers need the Python DLL directory on
    # PATH. Windows cross actions instead need the explicitly configured
    # host path, which takes precedence when cross mode is enabled.
    action_env = py_action_env_windows_dll_path(ctx)
    windows_cross_host_path = ctx.attr._windows_cross_host_path[BuildSettingInfo].value
    if windows_cross_host_path:
        action_env["PATH"] = windows_cross_host_path

    expanded_extra_definitions = {}
    for key, val in ctx.attr.extra_definitions.items():
        # Bazel throws an error if you try to call this on a location var
        if "$(location" not in val:
            expanded_extra_definitions |= {
                key: ctx.expand_make_variables("generate_config_header_expand", val, ctx.var),
            }

    expanded_extra_definitions |= {
        "compile_variables": " ".join(compiler_flags + ctx.attr.cpp_opts),
        "linkflags": " ".join(link_flags + ctx.attr.cpp_linkflags),
        "cpp_defines": " ".join(ctx.attr.cpp_defines),
    }

    # Config-header actions execute on the execution platform, which may differ from the
    # target platform for cross-compilation. Pass the target values explicitly so generators
    # do not accidentally embed execution-worker metadata.
    target_arch = _target_platform_arch(ctx)
    expanded_extra_definitions |= {
        "MONGO_DISTARCH": target_arch,
        "TARGET_ARCH": target_arch,
        "TARGET_OS": _target_platform_os(ctx),
    }

    additional_inputs = []
    additional_inputs_depsets = []
    for additional_input in ctx.attr.additional_inputs:
        files = additional_input.files.to_list()
        additional_inputs_depsets.append(additional_input.files)
        for file in files:
            additional_inputs.append("--additional-input")
            additional_inputs.append(file.path)

    ctx.actions.run(
        executable = ctx.executable.generator_script,
        outputs = [ctx.outputs.output, ctx.outputs.logfile],
        mnemonic = "ConfigHeaderGen",
        inputs = depset(transitive = [
            cc_toolchain.all_files,
            ctx.attr.template.files,
            ctx.attr.checks.files,
        ] + additional_inputs_depsets),
        arguments = [
                        "--output-path",
                        ctx.outputs.output.path,
                        "--template-path",
                        input,
                        "--check-path",
                        checks,
                        "--log-path",
                        ctx.outputs.logfile.path,
                        "--compiler-path",
                        compiler_bin,
                        "--extra-definitions",
                        json.encode(expanded_extra_definitions),
                    ] +
                    additional_inputs +
                    [
                        "--compiler-args",
                        " ".join(compiler_flags),
                        "--env-vars",
                        json.encode(env_flags | {"SDKROOT": ctx.attr._sdkroot[sdkroot_provider].path}),
                    ],
        env = action_env,
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

generate_config_header_rule = rule(
    generate_config_header_impl,
    attrs = {
        "output": attr.output(
            doc = "The output of this rule.",
            mandatory = True,
        ),
        "logfile": attr.output(
            doc = "The logfile of this rule.",
            mandatory = True,
        ),
        "template": attr.label(
            doc = "The template file used to generate the header.",
            allow_single_file = True,
        ),
        "checks": attr.label(
            doc = "The input checks python script to run for this rule.",
            allow_single_file = True,
        ),
        "extra_definitions": attr.string_dict(
            doc = "Extra definitions to set.",
            default = {},
        ),
        "additional_inputs": attr.label_list(
            doc = "Additional inputs to this rule.",
            allow_files = True,
        ),
        "cpp_linkflags": attr.string_list(
            doc = "C++ linkflags.",
        ),
        "cpp_opts": attr.string_list(
            doc = "C++ opts.",
        ),
        "cpp_defines": attr.string_list(
            doc = "C++ defines.",
        ),
        "generator_script": attr.label(
            doc = "The python generator script to use.",
            default = "//bazel/config:generate_config_header",
            executable = True,
            cfg = "exec",
        ),
        "_cc_toolchain": attr.label(default = "@rules_cc//cc:current_cc_toolchain"),
        "_sdkroot": attr.label(default = "//bazel/config:sdkroot"),
        "_windows_cross_host_path": attr.label(default = "//bazel/config:windows_cross_host_path"),
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
    output_to_genfiles = True,
)

def generate_config_header(name, tags = [], **kwargs):
    generate_config_header_rule(
        name = name,
        tags = tags + ["gen_source"],
        **kwargs
    )
