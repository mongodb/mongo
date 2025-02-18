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
