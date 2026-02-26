load("//bazel:utils.bzl", "write_target")
load("@rules_python//python:defs.bzl", "py_binary")

def render_template_impl(ctx):
    expanded_args = [
        ctx.expand_make_variables("render_template_expand", ctx.expand_location(arg, ctx.attr.srcs), ctx.var)
        for arg in ctx.attr.cmd
    ]

    # Add runfiles package dir to PYTHONPATH so scripts can import python_libs (e.g. gen_helper).
    runfiles_package_dir = ctx.executable.python_binary.path + ".runfiles/" + ctx.workspace_name + "/" + ctx.label.package
    env = {"PYTHONPATH": runfiles_package_dir}

    ctx.actions.run(
        executable = ctx.executable.python_binary,
        outputs = [ctx.outputs.output],
        inputs = depset(transitive = [depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])]),
        arguments = expanded_args,
        env = env,
        mnemonic = "TemplateRenderer",
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
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
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
    env = {"PYTHONPATH": runfiles_package_dir}

    ctx.actions.run(
        executable = ctx.executable.python_binary,
        outputs = ctx.outputs.outputs,
        inputs = depset(transitive = [depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])]),
        arguments = expanded_args,
        env = env,
        mnemonic = "TemplateRenderer",
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
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
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
