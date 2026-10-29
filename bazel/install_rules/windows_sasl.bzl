"""Repository rule for the prebuilt Windows Cyrus SASL bundle.

This is a plain download-and-extract, but it cannot be a bare http_archive with
static build_file_content: the generated BUILD file has to name the repository's
own on-disk directory in a /LIBPATH: linker flag, and under bzlmod that directory
is the mangled canonical name (e.g. _main~_repo_rules~windows_sasl) rather than
the apparent name. The directory is only knowable at fetch time, via
ctx.path("."), so the BUILD file is generated here with it substituted in.

The include path does not need the same treatment — `includes = ["include"]` on
the cc_library lets Bazel compute the -isystem flags itself, and
//src/third_party/sasl:windows_sasl is in MONGO_GLOBAL_SRC_DEPS, so every
mongo_cc_library picks it up transitively.
"""

load("//bazel:utils.bzl", "retry_download_and_extract")

URL = "https://s3.amazonaws.com/boxes.10gen.com/build/windows_cyrus_sasl-2.1.28.zip"
SHA256 = "3e22e2b16f802277123590f64dfda44f1c9c8a2b7e758180cd956d8ab0965817"

_BUILD_TEMPLATE = """
package(default_visibility = ["//visibility:public"])

# Consumers link against SASL by bare name (e.g. `linkopts = ["sasl2.lib"]` in
# src/mongo/client/BUILD.bazel and the enterprise sasl/ldap/streams packages),
# so the linker needs a search path rather than a direct input. {repo_dir} is
# this repository's canonical directory, substituted at fetch time.
cc_library(
    name = "sasl",
    hdrs = select({{
        "@platforms//os:windows": glob(["include/**/*.h"]),
        "//conditions:default": [],
    }}),
    includes = select({{
        "@platforms//os:windows": ["include"],
        "//conditions:default": [],
    }}),
    linkopts = select({{
        "@platforms//os:windows": ["/LIBPATH:external/{repo_dir}/lib"],
        "//conditions:default": [],
    }}),
    additional_linker_inputs = select({{
        "@platforms//os:windows": glob(["lib/**/*"]),
        "//conditions:default": [],
    }}),
)

filegroup(
    name = "includes",
    srcs = select({{
        "@platforms//os:windows": glob(["include/**/*.h"]),
        "//conditions:default": [],
    }}),
)

filegroup(
    name = "libraries",
    srcs = select({{
        "@platforms//os:windows": glob(["lib/**/*"]),
        "//conditions:default": [],
    }}),
)

filegroup(
    name = "bins",
    srcs = select({{
        "@platforms//os:windows": glob(["bin/**/*"]),
        "//conditions:default": [],
    }}),
)
"""

def _windows_sasl_impl(ctx):
    ctx.report_progress("downloading windows cyrus sasl")
    retry_download_and_extract(
        ctx = ctx,
        tries = 5,
        url = [URL] * 5,
        sha256 = SHA256,
    )

    # ctx.path(".") is the repository root; its basename is the canonical
    # repository name, which is what appears under execroot/external.
    ctx.file(
        "BUILD.bazel",
        _BUILD_TEMPLATE.format(repo_dir = ctx.path(".").basename),
    )

windows_sasl = repository_rule(
    implementation = _windows_sasl_impl,
    attrs = {},
)
