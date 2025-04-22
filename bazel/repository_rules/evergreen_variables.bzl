def _setup_evergreen_variables(ctx):
    compile_variant = ctx.os.environ.get("compile_variant")
    version_id = ctx.os.environ.get("version_id")

    ctx.file(
        "BUILD.bazel",
        "",
    )
    ctx.file(
        "evergreen_variables.bzl",
        """
UNSAFE_COMPILE_VARIANT = "%s"
UNSAFE_VERSION_ID = "%s"
""" % (compile_variant, version_id),
    )

setup_evergreen_variables = repository_rule(
    implementation = _setup_evergreen_variables,
    environ = ["compile_variant", "version_id"],
)
