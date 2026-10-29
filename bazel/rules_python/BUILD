# Patches applied to rules_python via single_version_override in
# MODULE.bazel.

load("@rules_python//python:defs.bzl", "py_test")

exports_files([
    "bootstrap_runfiles_manifest.patch",
    "bootstrap_windows_venv_robustness.patch",
    "host_platform_ppc64le.patch",
    "site_init_windows_long_paths.patch",
    "stage2_bootstrap_runfiles_manifest.patch",
])

py_test(
    name = "site_init_windows_long_paths_test",
    srcs = ["site_init_windows_long_paths_test.py"],
    args = ["$(location @rules_python//python/private:site_init_template)"],
    data = ["@rules_python//python/private:site_init_template"],
    tags = ["unit"],
    visibility = ["//visibility:public"],
)

py_test(
    name = "bootstrap_runfiles_manifest_test",
    srcs = ["bootstrap_runfiles_manifest_test.py"],
    args = [
        "$(location @rules_python//python/private:python_bootstrap_template.txt)",
        "$(location @rules_python//python/private:stage2_bootstrap_template)",
    ],
    data = [
        "@rules_python//python/private:python_bootstrap_template.txt",
        "@rules_python//python/private:stage2_bootstrap_template",
    ],
    tags = ["unit"],
)
