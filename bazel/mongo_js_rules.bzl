"""Macros wrapping rules_js's javascript rules."""

load("@aspect_rules_js//js:defs.bzl", "js_library")

def mongo_js_library(*args, **kwargs):
    if "target_compatible_with" not in kwargs:
        kwargs["target_compatible_with"] = []
    kwargs["target_compatible_with"] += select({
        "//bazel/config:ppc_or_s390x": ["@platforms//:incompatible"],
        "//conditions:default": [],
    })
    js_library(*args, **kwargs)

def all_subpackage_javascript_files(name = "all_subpackage_javascript_files", exclude_subpackages = []):
    """Creates a js_library containing all .js sources from all Bazel subpackages that also contain the all_subpackage_javascript_files macro.

    Subpackages listed in exclude_subpackages are skipped; use this for subpackages that do not
    declare this target (e.g. directories of resmoke_suite_test targets rather than test sources).
    """
    subpackage_targets = ["//{}/{}:{}".format(native.package_name(), subpackage, name) for subpackage in native.subpackages(include = ["**"], exclude = [".auto_header"] + exclude_subpackages, allow_empty = True)]

    mongo_js_library(
        name = name,
        srcs = native.glob([
            "*.js",
        ]) + subpackage_targets,
    )
