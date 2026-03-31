"""Repository rule that fetches a pre-built SpiderMonkey WASI distribution.

Downloads and extracts the tarball, then generates a BUILD.bazel that exposes
SpiderMonkey headers, static libraries, and extra objects as proper Bazel targets.
"""

def _spidermonkey_wasi_impl(rctx):
    rctx.download_and_extract(
        url = rctx.attr.urls,
        sha256 = rctx.attr.sha256,
    )

    # Strip FMT_* redefinitions from js-confdefs.h so it can be safely
    # force-included without breaking MongoDB's own fmt headers.
    confdefs = rctx.path("include/js-confdefs.h")
    content = rctx.read(confdefs)
    filtered = "\n".join([
        line
        for line in content.split("\n")
        if not line.strip().startswith("#define FMT_")
    ])
    rctx.file("include/js-confdefs.h", content = filtered)

    # Detect which optional libraries exist in the tarball.
    has_jsrust = rctx.path("lib/libjsrust.a").exists

    jsrust_target = ""
    jsrust_dep = ""
    if has_jsrust:
        jsrust_target = """
cc_import(
    name = "jsrust",
    static_library = "lib/libjsrust.a",
)
"""
        jsrust_dep = '":jsrust",'

    rctx.file("BUILD.bazel", content = """
load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "headers",
    hdrs = glob(
        ["include/**/*.h", "include/**/*.hpp"],
        # Exclude vendored fmt headers that conflict with MongoDB's
        # own copy in //src/third_party/fmt.
        exclude = ["include/fmt/**"],
    ),
    defines = ["MOZ_HAS_MOZGLUE"],
    includes = ["include", "include/src"],
    textual_hdrs = glob(["include/**/*.msg", "include/**/*.tbl", "include/**/*.inc"]),
)

cc_import(
    name = "js_static",
    static_library = "lib/libjs_static.a",
)

cc_import(
    name = "rust_shims",
    static_library = "lib/libmongo_wasip2_rust_shims.a",
)

{jsrust_target}

# Extra prebuilt objects from SpiderMonkey's memory/mozglue/mfbt that are
# not included in libjs_static.a but required at link time.
cc_library(
    name = "obj_extra",
    srcs = glob(["obj-extra/**/*.o"]),
    linkstatic = True,
)

cc_library(
    name = "spidermonkey",
    deps = [
        ":headers",
        ":js_static",
        ":rust_shims",
        ":obj_extra",
        {jsrust_dep}
    ],
)
""".format(
        jsrust_target = jsrust_target,
        jsrust_dep = jsrust_dep,
    ))

spidermonkey_wasi = repository_rule(
    implementation = _spidermonkey_wasi_impl,
    attrs = {
        "urls": attr.string_list(mandatory = True),
        "sha256": attr.string(mandatory = True),
    },
)
