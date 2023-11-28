load("@bazel_tools//tools/python:toolchain.bzl", "py_runtime_pair")

filegroup(
    name = "files",
    srcs = glob(["**/*"]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "interpreter",
    srcs = ["{interpreter_path}"],
    visibility = ["//visibility:public"],
)

py_runtime(
    name = "py_runtime",
    files = [":files"],
    interpreter = ":interpreter",
    python_version = "PY3",
    visibility = ["//visibility:public"],
)

py_runtime_pair(
    name = "py_runtime_pair",
    py2_runtime = None,
    py3_runtime = ":py_runtime",
)

toolchain(
    name = "python_toolchain",
    exec_compatible_with = [{constraints}],
    target_compatible_with = [{constraints}],
    toolchain = ":py_runtime_pair",
    toolchain_type = "@bazel_tools//tools/python:toolchain_type",
)