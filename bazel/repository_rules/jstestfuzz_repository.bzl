"""Fetches the private 10gen/jstestfuzz repo via the canonical git_repository rule.

To fetch a specific commit without editing MODULE.bazel:
    bazel sync --repo_env=JSTESTFUZZ_COMMIT=abc123def456...
"""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

_BUILD_FILE_CONTENT = """\
package(default_visibility = ["//visibility:public"])

# The whole prepared checkout (sources + installed node_modules + compiled
# grammars + .jstestfuzz_commit_sha) as a single tarball. A tarball (rather
# than a glob filegroup) because node_modules contains symlinks.
filegroup(
    name = "bundle",
    srcs = ["jstestfuzz_bundle.tar"],
)
"""

# The commit the deterministic fuzzer suites pin:
_DETERMINISTIC_COMMIT = "101ae461ed7704ae4d8b5000cc43a6080ec1a804"

def _fetch(name, branch, commit):
    git_repository(
        name = name,
        remote = "https://github.com/10gen/jstestfuzz.git",
        branch = branch,
        commit = commit,
        build_file_content = _BUILD_FILE_CONTENT,
        # Preserves the fetched commit before git_repository strips .git/, since some
        # consumers (e.g. jstestfuzz's file_namer.ts) shell out to `git rev-parse HEAD`
        # at runtime.
        patch_cmds = [
            "git rev-parse HEAD > .jstestfuzz_commit_sha",
            "export TMP=${TMP:-${TMPDIR:-/tmp}}; export TMPDIR=$TMP; " +
            "python3 src/scripts/npm_run.py compile",
            # Bundle the prepared checkout into a single tarball (symlinks
            # preserved) that jstestfuzz_generate ships and unpacks per action.
            # Exclude .git and the node runtime download (node comes from the
            # bazel toolchain); exclude the bundle itself.
            "tar cf jstestfuzz_bundle.tar " +
            "--exclude=./.git --exclude=./node-v[0-9]* --exclude=./out " +
            "--exclude=./jstestfuzz_bundle.tar .",
        ],
    )

def _jstestfuzz_repository_impl(module_ctx):
    commit = module_ctx.getenv("JSTESTFUZZ_COMMIT", "").strip()
    _fetch(
        name = "jstestfuzz",
        branch = None if commit else "master",
        commit = commit or None,
    )

    _fetch(
        name = "jstestfuzz_deterministic",
        branch = None,
        commit = _DETERMINISTIC_COMMIT,
    )

jstestfuzz_repository = module_extension(
    implementation = _jstestfuzz_repository_impl,
    doc = (
        "Fetches 10gen/jstestfuzz via git_repository. Set JSTESTFUZZ_COMMIT to override " +
        "the fetched ref without editing MODULE.bazel."
    ),
)
