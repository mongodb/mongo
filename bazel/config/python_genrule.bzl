load("//bazel:utils.bzl", "write_target")

def python_genrule_impl(ctx):
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    python_libs = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.python_libs]

    python_path = []
    for py_dep in ctx.attr.python_libs:
        for path in py_dep[PyInfo].imports.to_list():
            if path not in python_path:
                python_path.append(ctx.expand_make_variables("python_library_imports", "$(BINDIR)/external/" + path, ctx.var))

    expanded_args = [
        ctx.expand_make_variables("render_template_expand", ctx.expand_location(arg, ctx.attr.srcs), ctx.var)
        for arg in ctx.attr.cmd
    ]

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = ctx.outputs.outputs,
        inputs = depset(transitive = [python.files, depset([arg.files.to_list()[0] for arg in ctx.attr.srcs])] + python_libs),
        arguments = expanded_args,
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
        mnemonic = "TemplateRenderer",
    )

    return [DefaultInfo(files = depset(ctx.outputs.outputs))]

python_genrule_rule = rule(
    python_genrule_impl,
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
        "python_libs": attr.label_list(
            providers = [PyInfo],
            default = [],
        ),
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    output_to_genfiles = True,
)

def python_genrule(name, tags = [], **kwargs):
    python_genrule_rule(
        name = name,
        tags = tags + ["gen_source"],
        **kwargs
    )
