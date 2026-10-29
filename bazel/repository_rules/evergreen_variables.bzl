def _setup_evergreen_variables(ctx):
    compile_variant = ctx.os.environ.get("compile_variant")
    version_id = ctx.os.environ.get("version_id")
    compile_task_type = ctx.os.environ.get("compile_task_type") or ""

    ctx.file(
        "BUILD.bazel",
        "",
    )
    ctx.file(
        "evergreen_variables.bzl",
        """
UNSAFE_COMPILE_VARIANT = "%s"
UNSAFE_VERSION_ID = "%s"
UNSAFE_COMPILE_TASK_TYPE = "%s"
""" % (compile_variant, version_id, compile_task_type),
    )

setup_evergreen_variables = repository_rule(
    implementation = _setup_evergreen_variables,
    environ = ["compile_variant", "version_id", "compile_task_type"],
)
