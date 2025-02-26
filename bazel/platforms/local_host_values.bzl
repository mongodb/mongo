def _local_host_values(ctx):
    if "win" in ctx.os.name:
        result = ctx.execute([
            ctx.path(ctx.attr.python_interpreter_target_win),
            "-c",
            "import os; print(os.cpu_count())",
        ])
    else:
        result = ctx.execute([
            ctx.path(ctx.attr.python_interpreter_target_default),
            "-c",
            "import os; print(os.cpu_count())",
        ])

    ctx.file(
        "BUILD.bazel",
        "",
    )
    ctx.file(
        "local_host_values_set.bzl",
        """
NUM_CPUS = %s
""" % (result.stdout),
    )

setup_local_host_values = repository_rule(
    implementation = _local_host_values,
    attrs = {
        "python_interpreter_target_default": attr.label(
            default = "@py_host//:dist/bin/python3",
            doc = "The target of the Python interpreter used during repository setup, if not windows",
        ),
        "python_interpreter_target_win": attr.label(
            default = "@py_host//:dist/python.exe",
            doc = "The target of the Python interpreter used during repository setup for windows platforms",
        ),
    },
)
