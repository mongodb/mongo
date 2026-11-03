load(
    "//bazel/config:py_action_env.bzl",
    "py_action_env_pythonpath",
    "py_action_env_windows_dll_path",
    "py_linux_python_action",
)
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_python//python:py_info.bzl", "PyInfo")
load("@rules_python//python:defs.bzl", "py_binary")

def _python_action_env(ctx, python_path):
    env = py_action_env_pythonpath(ctx, ctx.attr.python_binary, python_path)
    windows_python_env = py_action_env_windows_dll_path(ctx)
    env.update(windows_python_env)
    windows_cross_host_path = ctx.attr._windows_cross_host_path[BuildSettingInfo].value

    # Linux execution runtimes must keep the worker's PATH rather than the client path.
    if windows_cross_host_path and windows_python_env:
        env["PATH"] = windows_cross_host_path
    return env

def _template_renderer_action(ctx, expanded_args, outputs):
    use_windows_cross = ctx.attr._windows_cross_local_container_actions[BuildSettingInfo].value
    tools = []
    if use_windows_cross:
        # Windows py_binary launchers cannot run in the Linux RBE container.
        # Invoke the generator source with the Linux cross Python runtime.
        python_interpreter, python_source, python_files, env = py_linux_python_action(
            ctx,
            ctx.attr.python_binary,
            ctx.attr._linux_x86_64_python_interpreter,
            ctx.attr._linux_x86_64_python_files,
        )
        executable = python_interpreter
        arguments = [python_source.path.replace("\\", "/")] + expanded_args

        # Keep the tool's runfiles tree for helper modules and data that are not
        # PyInfo sources. Declaring files_to_run stages it even though we bypass
        # the Windows launcher and invoke the source with Linux Python.
        tools = [ctx.attr.python_binary[DefaultInfo].files_to_run]
        runfiles_dir = ctx.executable.python_binary.path.replace("\\", "/") + ".runfiles"
        runfiles_package_dir = runfiles_dir + "/" + ctx.workspace_name + "/" + ctx.label.package
        env["PYTHONPATH"] = runfiles_package_dir + ":" + env["PYTHONPATH"]
        env["RUNFILES_DIR"] = runfiles_dir
        inputs = depset(transitive = [
            depset([arg.files.to_list()[0] for arg in ctx.attr.srcs]),
            ctx.attr.python_binary[PyInfo].transitive_sources,
            depset([python_source]),
            python_files,
        ])
    else:
        # Add the runfiles package dir and dependency import paths so scripts
        # can import python_libs (e.g. gen_helper).
        runfiles_package_dir = ctx.executable.python_binary.path + ".runfiles/" + ctx.workspace_name + "/" + ctx.label.package
        env = _python_action_env(ctx, runfiles_package_dir)
        executable = ctx.executable.python_binary
        arguments = expanded_args
        inputs = depset(transitive = [depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])])

    ctx.actions.run(
        executable = executable,
        tools = tools,
        outputs = outputs,
        inputs = inputs,
        arguments = arguments,
        env = env,
        mnemonic = "TemplateRenderer",
        use_default_shell_env = True,
    )

def render_template_impl(ctx):
    expanded_args = [
        ctx.expand_make_variables("render_template_expand", ctx.expand_location(arg, ctx.attr.srcs), ctx.var)
        for arg in ctx.attr.cmd
    ]

    _template_renderer_action(ctx, expanded_args, [ctx.outputs.output])

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

def _python_generator_attrs():
    return {
        "srcs": attr.label_list(
            doc = "The input files of this rule.",
            allow_files = True,
        ),
        "cmd": attr.string_list(
            doc = "The command line arguments to pass to python",
        ),
        "python_binary": attr.label(
            executable = True,
            cfg = "exec",
        ),
        "_windows_cross_local_container_actions": attr.label(
            default = "//bazel/config:windows_cross_local_container_actions",
        ),
        "_windows_cross_host_path": attr.label(
            default = "//bazel/config:windows_cross_host_path",
        ),
        "_linux_x86_64_python_files": attr.label(
            default = "@py_linux_x86_64//:files",
        ),
        "_linux_x86_64_python_interpreter": attr.label(
            default = "@py_linux_x86_64//:interpreter",
        ),
    }

render_template_rule = rule(
    render_template_impl,
    attrs = _python_generator_attrs() | {
        "output": attr.output(
            doc = "The output of this rule.",
            mandatory = True,
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

    _template_renderer_action(ctx, expanded_args, ctx.outputs.outputs)

    return [DefaultInfo(files = depset(ctx.outputs.outputs))]

render_templates_rule = rule(
    render_templates_impl,
    attrs = _python_generator_attrs() | {
        "outputs": attr.output_list(
            doc = "The outputs of this rule.",
            mandatory = True,
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
