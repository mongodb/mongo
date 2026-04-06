load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

def _strip(s):
    return s.strip(" \t\r\n")

def _basename_no_git(url):
    u = url
    if u.endswith("/"):
        u = u[:-1]
    if u.endswith(".git"):
        u = u[:-4]

    # last path component
    parts = u.split("/")
    return parts[-1] if parts else "repo"

def _spidermonkey_repository_impl(rctx):
    repo_url = _strip(rctx.read(rctx.attr.repository_file))
    version = _strip(rctx.read(rctx.attr.version_file))
    sha256 = _strip(rctx.read(rctx.attr.sha256_file))

    if not repo_url:
        fail("spidermonkey_repository: repository_file was empty")
    if not version:
        fail("spidermonkey_repository: version_file was empty")
    if not sha256:
        fail("spidermonkey_repository: sha256_file was empty")

    # Normalize.
    if repo_url.endswith("/"):
        repo_url = repo_url[:-1]
    if repo_url.endswith(".git"):
        repo_url = repo_url[:-4]

    repo_name = _basename_no_git(repo_url)

    # GitHub archive URL for tags.
    # Example: https://github.com/mozilla-firefox/firefox/archive/refs/tags/FIREFOX_140_6_0esr_RELEASE.tar.gz
    archive_url = "{}/archive/refs/tags/{}.tar.gz".format(repo_url, version)
    strip_prefix = "{}-{}".format(repo_name, version)

    rctx.download_and_extract(
        url = archive_url,
        sha256 = sha256,
        stripPrefix = strip_prefix,
    )

    # Minimal BUILD file so downstream genrules can depend on `:mach` and `:srcs`.
    rctx.file(
        "BUILD.bazel",
        content = """
package(default_visibility = ["//visibility:public"])

exports_files(["mach"])

filegroup(
    name = "srcs",
    srcs = glob(
        ["**"],
        exclude = [
            "**/.git/**",
            "**/.hg/**",
        ],
    ),
)
""",
    )

spidermonkey_repository = repository_rule(
    implementation = _spidermonkey_repository_impl,
    attrs = {
        # Labels in the main workspace that contain the repo URL and tag/version.
        "repository_file": attr.label(mandatory = True, allow_single_file = True),
        "version_file": attr.label(mandatory = True, allow_single_file = True),
        "sha256_file": attr.label(mandatory = True, allow_single_file = True),
    },
    doc = "Downloads SpiderMonkey/Firefox source from a repo+tag stored in files, and exposes `:mach` and `:srcs`.",
)
