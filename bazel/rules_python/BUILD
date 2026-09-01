# Patches applied to rules_python via single_version_override in
# MODULE.bazel.

load("@rules_python//python:defs.bzl", "py_test")

exports_files([
    "bootstrap_windows_venv_robustness.patch",
    "host_platform_ppc64le.patch",
    "site_init_windows_long_paths.patch",
])

py_test(
    name = "site_init_windows_long_paths_test",
    srcs = ["site_init_windows_long_paths_test.py"],
    data = ["site_init_windows_long_paths.patch"],
    tags = ["unit"],
    visibility = ["//visibility:public"],
)
