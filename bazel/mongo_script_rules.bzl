"""Common mongo-specific bazel build rules intended to be used for buildscripts.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

MONGO_TOOLCHAIN_V4_PATH = "/opt/mongodbtoolchain/v4"
MONGO_TOOLCHAIN_V5_PATH = "external/mongo_toolchain_v5/v5"

def _py_cxx_wrapper(*, python_path, toolchain_path, python_interpreter, main_py):
    return "\n".join([
        "export PYTHONPATH={}".format(python_path),
        "export MONGO_TOOLCHAIN_PATH={}".format(toolchain_path),
        "{} {}".format(python_interpreter, main_py),
    ])

def _py_cxx_test_impl(ctx):
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime

    python_path = []
    for dep in ctx.attr.deps:
        for path in dep[PyInfo].imports.to_list():
            if path not in python_path:
                python_path.append(
                    ctx.expand_make_variables(
                        "python_library_imports",
                        "$${RUNFILES_DIR}/" + path,
                        ctx.var,
                    ),
                )
    python_path_str = ctx.configuration.host_path_separator.join(python_path)

    cc_toolchain = find_cpp_toolchain(ctx)
    runfiles = ctx.runfiles(
        files = (
            ctx.files.srcs +
            ctx.files.data +
            ctx.files.deps +
            ctx.files.main +
            cc_toolchain.all_files.to_list()
        ),
    )
    transitive_runfiles = []
    for runfiles_attr in (
        [ctx.attr.main],
        ctx.attr.srcs,
        ctx.attr.deps,
        ctx.attr.data,
    ):
        for target in runfiles_attr:
            transitive_runfiles.append(target[DefaultInfo].default_runfiles)
    runfiles = runfiles.merge_all(transitive_runfiles)

    main_py = ctx.attr.main.files.to_list()[0].path
    script = _py_cxx_wrapper(
        python_path = python_path_str,
        toolchain_path = ctx.attr.toolchain_path,
        python_interpreter = python.interpreter.path,
        main_py = main_py,
    )
    ctx.actions.write(
        output = ctx.outputs.executable,
        content = script,
    )

    return DefaultInfo(files = depset([ctx.outputs.executable]), runfiles = runfiles)

py_cxx_test = rule(
    implementation = _py_cxx_test_impl,
    attrs = {
        "main": attr.label(allow_single_file = True, mandatory = True),
        "srcs": attr.label_list(allow_files = [".py"]),
        "deps": attr.label_list(),
        "data": attr.label_list(),
        "toolchain_path": attr.string(mandatory = True),
    },
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type", "@bazel_tools//tools/python:toolchain_type"],
    executable = True,
    test = True,
)

def mongo_toolchain_py_cxx_test(**kwargs):
    py_cxx_test(
        toolchain_path = select({
            "//bazel/config:mongo_toolchain_v5": MONGO_TOOLCHAIN_V5_PATH,
            "//conditions:default": MONGO_TOOLCHAIN_V4_PATH,
        }),
        target_compatible_with = ["@//bazel/platforms:use_mongo_toolchain"],
        **kwargs
    )
