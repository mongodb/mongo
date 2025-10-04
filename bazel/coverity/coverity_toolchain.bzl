def _coverity_toolchain(ctx):
    retCode = 1
    if "COVERITY_INSTALL_ROOT" in ctx.os.environ:
        result = ctx.execute([
            "ls",
            ctx.getenv("COVERITY_INSTALL_ROOT") + "/bin/cov-build",
        ])
        retCode = result.return_code

    if retCode == 0:
        ctx.report_progress("extracting coverity rules...")
        result = ctx.download_and_extract("file://" + ctx.getenv("COVERITY_INSTALL_ROOT") + "/bazel/rules_coverity.tar.gz")
    else:
        ctx.template(
            "coverity/BUILD.bazel",
            ctx.attr.build_tpl,
        )
        ctx.template(
            "coverity/repositories.bzl",
            ctx.attr.repositories_tpl,
        )

coverity_toolchain = repository_rule(
    implementation = _coverity_toolchain,
    attrs = {
        "build_tpl": attr.label(
            default = "//bazel/coverity:coverity_toolchain.BUILD",
            doc = "Label denoting the BUILD file template that gets installed in the repo.",
        ),
        "repositories_tpl": attr.label(
            default = "//bazel/coverity:repositories.bzl",
            doc = "Label denoting the repositories files the gets installed to the repo.",
        ),
    },
)
