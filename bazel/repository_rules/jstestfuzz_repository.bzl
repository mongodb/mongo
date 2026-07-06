"""Fetches the private 10gen/jstestfuzz repo via the canonical git_repository rule.

Evergreen installs a git url.insteadOf credential rewrite for this repo's remote before
Bazel runs (see evergreen/write_github_repo_credentials.sh); this extension does not
handle authentication itself.

To fetch a specific commit without editing MODULE.bazel:
    bazel sync --repo_env=JSTESTFUZZ_COMMIT=abc123def456...
"""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

_BUILD_FILE_CONTENT = """\
package(default_visibility = ["//visibility:public"])

filegroup(
    name = "sources",
    srcs = glob(
        include = ["**/*"],
        exclude = [
            "node_modules/**",
            "out/**",
        ],
        allow_empty = True,
    ),
)
"""

def _jstestfuzz_repository_impl(module_ctx):
    commit = module_ctx.getenv("JSTESTFUZZ_COMMIT", "").strip()
    git_repository(
        name = "jstestfuzz",
        remote = "https://github.com/10gen/jstestfuzz.git",
        branch = None if commit else "master",
        commit = commit or None,
        build_file_content = _BUILD_FILE_CONTENT,
        # Preserves the fetched commit before git_repository strips .git/, since some
        # consumers (e.g. jstestfuzz's file_namer.ts) shell out to `git rev-parse HEAD`
        # at runtime.
        patch_cmds = ["git rev-parse HEAD > .jstestfuzz_commit_sha"],
    )

jstestfuzz_repository = module_extension(
    implementation = _jstestfuzz_repository_impl,
    doc = (
        "Fetches 10gen/jstestfuzz via git_repository. Set JSTESTFUZZ_COMMIT to override " +
        "the fetched ref without editing MODULE.bazel. Requires credentials already " +
        "configured for the system git (see evergreen/write_github_repo_credentials.sh)."
    ),
)
