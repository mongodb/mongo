# Patches applied to rules_pycross via single_version_override in
# MODULE.bazel.

load("@rules_python//python:defs.bzl", "py_test")
load(":wheel_library_tool_runfiles_test.bzl", "wheel_installer_runfiles_analysis_test")
load(":prebuilt_wheel_required.bzl", "prebuilt_wheel_required")
load(":prebuilt_wheel_required_test.bzl", "source_build_disabled_test")

exports_files([
    "installer_repo_extract.patch",
    "wheel_build_cargo_home.patch",
    "wheel_build_no_werror.patch",
    "crossenv_glibc_teller.patch",
    "crossenv_sys_platform_recursion.patch",
    "installer_tools_build.patch",
    "wheel_installer_patch_ng_runfiles.patch",
    "uv_translator_url_source.patch",
    "wheel_library_windows_dll_path.patch",
    "cc_toolchain_cross_exec_platform.patch",
    "wheel_build_cross_exec_platform.patch",
    "wheel_library_cross_exec_platform.patch",
    "target_environment_cross_exec_platform.patch",
    "wheel_library_tool_runfiles.patch",
])

wheel_installer_runfiles_analysis_test(
    name = "wheel_library_tool_runfiles_analysis_test",
    target_under_test = "@pypi//:dnspython",
)

py_test(
    name = "wheel_installer_patch_ng_runfiles_test",
    srcs = ["wheel_installer_patch_ng_runfiles_test.py"],
    args = ["$(location @rules_pycross//pycross/private/tools:wheel_installer_source)"],
    data = ["@rules_pycross//pycross/private/tools:wheel_installer_source"],
    tags = ["unit"],
)

prebuilt_wheel_required(
    name = "cryptography_source_build_disabled",
    package = "cryptography",
    tags = ["manual"],
    visibility = ["//visibility:public"],
)

source_build_disabled_test(
    name = "cryptography_source_build_disabled_test",
    target_under_test = ":cryptography_source_build_disabled",
)

py_test(
    name = "wheel_build_environment_test",
    srcs = ["wheel_build_environment_test.py"],
    args = ["$(location @rules_pycross//pycross/private/tools:wheel_builder_source)"],
    data = ["@rules_pycross//pycross/private/tools:wheel_builder_source"],
    tags = ["unit"],
)
