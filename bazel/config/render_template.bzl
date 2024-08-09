def render_template_impl(ctx):
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    python_libs = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.python_libs]

    python_path = []
    for py_dep in ctx.attr.python_libs:
        for dep in py_dep[PyInfo].transitive_sources.to_list():
            if dep.path not in python_path:
                python_path.append(dep.path)

    expanded_args = [
        ctx.expand_make_variables("render_template_expand", ctx.expand_location(arg, ctx.attr.srcs), ctx.var)
        for arg in ctx.attr.cmd
    ]

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [ctx.outputs.output],
        inputs = depset(transitive = [python.files, depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])] + python_libs),
        arguments = expanded_args,
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
        mnemonic = "TemplateRenderer",
    )
    print("Generated error codes file at: " + ctx.outputs.output.path)

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

render_template = rule(
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
        "python_libs": attr.label_list(
            providers = [PyInfo],
            default = [],
        ),
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    output_to_genfiles = True,
)
