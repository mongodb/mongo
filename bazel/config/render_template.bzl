load("//bazel:utils.bzl", "write_target")
load("//bazel/config:py_action_env.bzl", "py_action_env_windows_dll_path")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_python//python:defs.bzl", "py_binary")

def _python_action_env(ctx, python_path):
    env = {"PYTHONPATH": python_path}
    env.update(py_action_env_windows_dll_path(ctx))
    windows_cross_host_path = ctx.attr._windows_cross_host_path[BuildSettingInfo].value
    if windows_cross_host_path:
        env["PATH"] = windows_cross_host_path
    return env

def render_template_impl(ctx):
    expanded_args = [
        ctx.expand_make_variables("render_template_expand", ctx.expand_location(arg, ctx.attr.srcs), ctx.var)
        for arg in ctx.attr.cmd
    ]

    # Add runfiles package dir to PYTHONPATH so scripts can import python_libs (e.g. gen_helper).
    runfiles_package_dir = ctx.executable.python_binary.path + ".runfiles/" + ctx.workspace_name + "/" + ctx.label.package
    env = _python_action_env(ctx, runfiles_package_dir)

    ctx.actions.run(
        executable = ctx.executable.python_binary,
        outputs = [ctx.outputs.output],
        inputs = depset(transitive = [depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])]),
        arguments = expanded_args,
        env = env,
        mnemonic = "TemplateRenderer",
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

render_template_rule = rule(
    render_template_impl,
    attrs = {
        "srcs": attr.label_list(
            doc = "The input files of this rule.",
            allow_files = True,
        ),
        "output": attr.output(
            doc = "The output of this rule.",
            mandatory = True,
        ),
        "cmd": attr.string_list(
            doc = "The command line arguments to pass to python",
        ),
        "python_binary": attr.label(
            executable = True,
            cfg = "exec",
        ),
        "_windows_cross_host_path": attr.label(
            default = "//bazel/config:windows_cross_host_path",
        ),
    },
    toolchains = ["@rules_python//python:toolchain_type"],
    output_to_genfiles = True,
)

def render_template(name, srcs, cmd, output, python_file, python_libs = [], tags = [], **kwargs):
    py_binary(
        name = name + "_python",
        srcs = [python_file],
        main = python_file,
        tags = tags + ["gen_source"],
        deps = python_libs,
    )
    render_template_rule(
        name = name,
        srcs = srcs,
        cmd = cmd,
        output = output,
        python_binary = name + "_python",
        tags = tags + ["gen_source"],
        **kwargs
    )

def render_templates_impl(ctx):
    expanded_args = [
        ctx.expand_make_variables("render_template_expand", ctx.expand_location(arg, ctx.attr.srcs), ctx.var)
        for arg in ctx.attr.cmd
    ]

    # Add runfiles package dir to PYTHONPATH so scripts can import python_libs (e.g. gen_helper).
    runfiles_package_dir = ctx.executable.python_binary.path + ".runfiles/" + ctx.workspace_name + "/" + ctx.label.package
    env = _python_action_env(ctx, runfiles_package_dir)

    ctx.actions.run(
        executable = ctx.executable.python_binary,
        outputs = ctx.outputs.outputs,
        inputs = depset(transitive = [depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])]),
        arguments = expanded_args,
        env = env,
        mnemonic = "TemplateRenderer",
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset(ctx.outputs.outputs))]

render_templates_rule = rule(
    render_templates_impl,
    attrs = {
        "srcs": attr.label_list(
            doc = "The input files of this rule.",
            allow_files = True,
        ),
        "outputs": attr.output_list(
            doc = "The outputs of this rule.",
            mandatory = True,
        ),
        "cmd": attr.string_list(
            doc = "The command line arguments to pass to python",
        ),
        "python_binary": attr.label(
            executable = True,
            cfg = "exec",
        ),
        "_windows_cross_host_path": attr.label(
            default = "//bazel/config:windows_cross_host_path",
        ),
    },
    toolchains = ["@rules_python//python:toolchain_type"],
    output_to_genfiles = True,
)

def render_templates(name, srcs, cmd, outputs, python_file, python_libs = [], tags = [], **kwargs):
    py_binary(
        name = name + "_python",
        srcs = [python_file],
        main = python_file,
        tags = tags + ["gen_source"],
        deps = python_libs,
    )
    render_templates_rule(
        name = name,
        srcs = srcs,
        cmd = cmd,
        outputs = outputs,
        python_binary = name + "_python",
        tags = tags + ["gen_source"],
        **kwargs
    )
