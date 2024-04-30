def _coverity_toolchain(ctx):
    result = ctx.execute([
        "ls",
        "/data/cov-sa/bin/cov-build",
    ])

    if result.return_code == 0:
        ctx.report_progress("extracting coverity rules...")
        result = ctx.download_and_extract("file:///data/cov-sa/bazel/rules_coverity.tar.gz")
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
