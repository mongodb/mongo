load("//bazel:utils.bzl", "write_target")
load("@rules_python//python:defs.bzl", "py_binary")

def python_genrule_impl(ctx):
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
        "python_binary": attr.label(
            executable = True,
            cfg = "exec",
        ),
    },
    toolchains = ["@rules_python//python:toolchain_type"],
    output_to_genfiles = True,
)

def python_genrule(name, python_file, python_libs = [], tags = [], **kwargs):
    py_binary(
        name = name + "_python",
        srcs = [python_file],
        main = python_file,
        tags = tags + ["gen_source"],
        deps = python_libs,
    )
    python_genrule_rule(
        name = name,
        tags = tags + ["gen_source"],
        python_binary = name + "_python",
        **kwargs
    )
