def render_template_impl(ctx):
    template = ctx.attr.template.files.to_list()[0].path
    yml = ctx.attr.yml.files.to_list()[0].path
    generator_script = ctx.attr.generator_script.files.to_list()[0].path

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    python_libs = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.python_libs]

    python_path = []
    for py_dep in ctx.attr.python_libs:
        for dep in py_dep[PyInfo].transitive_sources.to_list():
            if dep.path not in python_path:
                python_path.append(dep.path)

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [ctx.outputs.output],
        inputs = depset(transitive = [python.files, ctx.attr.generator_script.files, ctx.attr.template.files, ctx.attr.yml.files] + python_libs),
        arguments = [
            generator_script,
            yml,
            template,
            ctx.outputs.output.path,
        ],
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
        mnemonic = "TemplateRenderer",
    )
    print("Generated error codes file at: " + ctx.outputs.output.path)

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

render_template = rule(
    render_template_impl,
    attrs = {
        "output": attr.output(
            doc = "The output of this rule.",
            mandatory = True,
        ),
        "template": attr.label(
            doc = "The header template of this rule.",
            allow_single_file = True,
        ),
        "yml": attr.label(
            doc = "The yml input of this rule.",
            allow_single_file = True,
        ),
        "generator_script": attr.label(
            doc = "The python generator script to use.",
            allow_single_file = True,
        ),
        "python_libs": attr.label_list(
            providers = [PyInfo],
            default = [],
        ),
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    output_to_genfiles = True,
)
