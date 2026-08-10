"""Non-hermetic build rule for fetching the private 10gen/query-correctness-tests-N corpora.

This is deliberately *not* a repository rule. Fetching at loading/analysis time
made every query over //... (cquery, `bazel build //...`) liable to
pull multiple GB of private test data that is only needed for test execution.
"""

def _query_correctness_corpus_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name + "/generated_tests")

    args = ctx.actions.args()
    args.add("--repo", ctx.attr.repo)
    args.add("--conf", ctx.file.conf)

    # The script writes <out>/generated_tests, so hand it the parent directory.
    args.add("--out", out.dirname)

    ctx.actions.run(
        outputs = [out],
        inputs = [ctx.file.conf],
        executable = ctx.executable._fetch,
        arguments = [args],
        mnemonic = "QueryCorrectnessCorpus",
        progress_message = "Fetching query correctness corpus %s" % ctx.attr.repo,
        # Non-hermetic: needs the network and the user's GitHub credentials.
        use_default_shell_env = True,
        execution_requirements = {
            "local": "1",
            "no-remote": "1",
            "no-sandbox": "1",
            "no-cache": "1",
            "requires-network": "1",
        },
    )

    return [DefaultInfo(files = depset([out]))]

query_correctness_corpus = rule(
    implementation = _query_correctness_corpus_impl,
    attrs = {
        "repo": attr.string(
            mandatory = True,
            doc = "Repo name under the 10gen org, e.g. 'query-correctness-tests-1'.",
        ),
        "conf": attr.label(
            allow_single_file = True,
            default = "//src/mongo/db/query/query_tester/tests:test_repos.conf",
            doc = "Config file pinning each corpus repo to a commit.",
        ),
        "_fetch": attr.label(
            default = "//bazel/resmoke:query_correctness_corpus_fetch",
            executable = True,
            cfg = "exec",
        ),
    },
    doc = (
        "Downloads the generated_tests/ tree of a 10gen/query-correctness-tests-N " +
        "repo at the commit pinned in test_repos.conf. Produces a directory " +
        "'<name>/generated_tests'. Non-hermetic: local execution only, uses the " +
        "network and system GitHub credentials."
    ),
)
